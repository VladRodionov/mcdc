/*
 * Copyright (c) 2025 Vladimir Rodionov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * mcz_compression.c
 *
 * Implementation of Zstd compression/decompression for MCZ.
 *
 * Key duties:
 *   - Manage Zstd CCtx/DCtx instances and dictionary attachments.
 *   - Provide fast-path compression and decompression entry points.
 *   - Handle integration with dictionary router table and trainer thread.
 *   - Maintain thread-local caches to reduce allocation churn.
 *
 * Notes:
 *   - Hot-path code avoids locks; relies on atomics and TLS.
 *   - Trainer thread may update global dictionary table asynchronously.
 *   - Always validate dictionary IDs and namespaces before use.
 */
#include "mcz_compression.h"
#define ZDICT_STATIC_LINKING_ONLY

#include <pthread.h>
#include <stdio.h>          /* FILE, fopen, fread, fclose */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>         /* usleep                     */
#include <zdict.h>          /* ZDICT_trainFromBuffer      */
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>         /* PATH_MAX                   */
#include <sys/types.h>
#include <sys/stat.h>
#include "memcached.h"
#include "mcz_incompressible.h"
#include "mcz_utils.h"
#include "mcz_gc.h"
#include "mcz_dict_pool.h"
#include "mcz_eff_atomic.h"
#include "mcz_sampling.h"


static __thread tls_cache_t tls; /* zero-initialised */

/* ---------- zstd context --------------------------------------------- */
mcz_ctx_t g_mcz = { 0 };      /* zero-init by the loader */

/* ---------- zstd context helpers ------------------------------------ */
const mcz_ctx_t *mcz_ctx(void)      { return &g_mcz; }
mcz_ctx_t       *mcz_ctx_mut(void)  { return &g_mcz; }


static const mcz_table_t *mcz_current_table(void) {
    const mcz_ctx_t *ctx = mcz_ctx();
    return (const mcz_table_t*)atomic_load_explicit(&ctx->dict_table, memory_order_acquire);
}

/* ---------- helper macros ------------------------------------------- */

#define KB(x)  ((size_t)(x) << 10)
#define MB(x)  ((size_t)(x) << 20)

/* sane absolute limits */
enum {
    ZSTD_LVL_MIN   = 1,
    ZSTD_LVL_MAX   = 22,
    ZSTD_DICT_MAX  = MB(1),   /* upstream hard-limit */
    ZSTD_VALUE_MAX = KB(200),  /* arbitrary safety cap, usually must be much less*/
    ZSTD_VALUE_MIN = 16        /* absolute min size of a value for compression */
};

static int attach_cfg(void)
{
    mcz_cfg_t *cfg =  mcz_config_get();
    mcz_ctx_t *ctx = mcz_ctx_mut();
    ctx->cfg = cfg;
    if (!cfg->enable_comp) {
        return -EINVAL;
    }
    /* 1. Compression level ---------------------------------------- */
    int lvl = cfg->zstd_level;
    if (lvl == 0)                           /* 0 = default (3) */
        lvl = 3;
    if (lvl < ZSTD_LVL_MIN || lvl > ZSTD_LVL_MAX) {
        if (settings.verbose > 1) {
            fprintf(stderr,
                    "ERROR: zstd level %d out of range [%d..%d]\n",
                    lvl, ZSTD_LVL_MIN, ZSTD_LVL_MAX);
        }
        return -EINVAL;
    }
    cfg->zstd_level = lvl;

    /* 2. Dictionary size ------------------------------------------ */
    size_t dict_sz = cfg->dict_size;
    if (dict_sz == 0) dict_sz = KB(112);      /* good default */
    if (dict_sz > ZSTD_DICT_MAX) dict_sz = ZSTD_DICT_MAX;
    cfg->dict_size = dict_sz;

    if (cfg->max_comp_size >= settings.slab_chunk_size_max){
        cfg->max_comp_size = settings.slab_chunk_size_max - 1;
        if (cfg->max_comp_size > ZSTD_VALUE_MAX){
            cfg->max_comp_size = ZSTD_VALUE_MAX;
        }
    }
    if (cfg->min_comp_size > cfg->max_comp_size ||
        cfg->max_comp_size > ZSTD_VALUE_MAX) {
        if (settings.verbose > 1) {
            fprintf(stderr,
                    "ERROR: invalid zstd min/max comp size (%zu / %zu)\n",
                    cfg->min_comp_size, cfg->max_comp_size);
        }
        return -EINVAL;
    }
    return 0;
}

static void tls_ensure(size_t need) {
    if (!tls.cctx)
        tls.cctx = ZSTD_createCCtx();
    if (!tls.dctx)
        tls.dctx = ZSTD_createDCtx();
    if (need > tls.cap) {
        tls.scratch = realloc(tls.scratch, need);
        tls.cap = need;
    }
}

/* ---------------------------------------------------------------------
 * load dictionaries from a FS
 *   If dict_path == NULL it returns 1 (nothing loaded, continue live training).
 *   On I/O/alloc/ZSTD errors returns a negative errno.
 * ------------------------------------------------------------------- */
static int mcz_load_dicts(void) {
    mcz_ctx_t *ctx = mcz_ctx_mut();
    if (!ctx->cfg->dict_dir)
        return 1; /* nothing to load, continue live training */
    if (!ctx->cfg->enable_dict){
        return 1;
    }
    char *err = NULL;
    mcz_table_t *tab = mcz_scan_dict_dir(ctx->cfg->dict_dir, ctx->cfg->dict_retain_max,
                                         ctx->cfg->gc_quarantine_period, ctx->cfg->zstd_level, &err);
    if (err != NULL){
        fprintf(stderr, "load dictionaries failed: %s\n", err ? err : "unknown error");
        free(err);
        return 1;
    }
    if (tab) {
        atomic_store_explicit(&ctx->dict_table, (uintptr_t)tab, memory_order_release);
    } else {
        return 1;
    }
    return 0;
}

static inline bool is_training_active(void) {
    const mcz_ctx_t *c = mcz_ctx();
    /* any cheap acquire fence is fine; on x86 it's a compiler barrier */
    atomic_thread_fence(memory_order_acquire);
    return c->train_active;
}

static inline void set_training_active(bool active){
    mcz_ctx_t *ctx = mcz_ctx_mut();
    atomic_store(&ctx->train_active, active);
}


static size_t train_fastcover(void* dictBuf, size_t dictCap,
                            const void* samplesBuf, const size_t* sampleSizes, unsigned nbSamples)
{

    size_t got = ZDICT_trainFromBuffer(
        dictBuf, dictCap, samplesBuf, sampleSizes, nbSamples);

    /* p now holds the chosen k,d,steps */
    return got; /* check ZDICT_isError(got) / ZDICT_getErrorName(got) */
}


static size_t train_fastcover_optimize(void* dictBuf, size_t dictCap,
                                const void* samplesBuf, const size_t* sampleSizes, unsigned nbSamples)
{
    int targetLevel = mcz_ctx()->cfg->zstd_level;
    ZDICT_fastCover_params_t p;                /* advanced; requires ZDICT_STATIC_LINKING_ONLY */
    memset(&p, 0, sizeof(p));                  /* 0 => defaults; also enables search for k/d/steps */
    /* Leave at 0 to ENABLE search (fastCover’s optimizer will vary these) */
    p.k     = 0;   /* segment size */
    p.d     = 0;   /* dmer size    */
    p.steps = 0;   /* number of k points to try */

    /* fastCover-specific knobs */
    p.f     = 0;   /* log2(feature-buckets). 0 = let optimizer choose; note memory ~ 6*2^f per thread */
    p.accel = 0;   /* 0 = default (1); higher is faster/less accurate */
    p.nbThreads = 1;       /* grows memory per thread */
    p.splitPoint = 0.0;    /* 0.0 → default 0.75/0.25 split */

    /* Optional shrink-to-fit dictionary selection */
    p.shrinkDict = 0;                 /* 1 = try smaller dict sizes */
    p.shrinkDictMaxRegression = 0;    /* % regression allowed vs max dict */

    /* Header / stats options */
    p.zParams.compressionLevel   = targetLevel;
    p.zParams.notificationLevel  = 0;
    p.zParams.dictID             = 0;

    size_t got = ZDICT_optimizeTrainFromBuffer_fastCover(
        dictBuf, dictCap, samplesBuf, sampleSizes, nbSamples, &p);

    return got; /* check ZDICT_isError(got) */
}

static size_t train_dictionary(void* dictBuf, size_t dictCap,
                                const void* samplesBuf, const size_t* sampleSizes, unsigned nbSamples)
{
    const mcz_ctx_t *ctx = mcz_ctx();
    mcz_train_mode_t mode = ctx->cfg->train_mode;
    if (mode == MCZ_TRAIN_FAST) {
        return train_fastcover(dictBuf, dictCap,
                                    samplesBuf, sampleSizes, nbSamples);
    } else {
        return train_fastcover_optimize(dictBuf, dictCap,
                                    samplesBuf, sampleSizes, nbSamples);
    }
}


/* ---------- trainer thread ------------------------------------------ */
static void* trainer_main(void *arg) {
    mcz_ctx_t *ctx = arg;
    const size_t max_dict = ctx->cfg->dict_size ? ctx->cfg->dict_size : 110 * 1024;
    const size_t train_threshold =
        ctx->cfg->min_training_size ? ctx->cfg->min_training_size : max_dict * 100; /* 100× rule */

    for (;;) {
        usleep(1000000); // 1000 ms

        bool need_training = false;
        bool success = false;

        /* Decide if training should be active (sticky until success/admin-off) */
        const mcz_table_t* tab = (const mcz_table_t*) atomic_load_explicit(&ctx->dict_table, memory_order_acquire);
        if(!mcz_has_default_dict(tab)) {
            need_training = true; /* bootstrap */
        } else if (mcz_eff_should_retrain((uint64_t)time(NULL))) {
            need_training = true;
        }

        if (need_training) set_training_active(true);
        if (!is_training_active()) continue;

        /* Threshold gate */
        size_t pending = atomic_load_explicit(&ctx->bytes_pending, memory_order_acquire);

        if (pending < train_threshold) continue;

        /* get statistics for "default" namespace*/
        mcz_stats_atomic_t * stats = mcz_stats_lookup_by_ns("default", 7);
        if (stats) atomic_inc64(&stats->trainer_runs, 1);

        /* Take ownership of sample list */
        sample_node_t *list = atomic_exchange_explicit(&ctx->samples_head, NULL, memory_order_acq_rel);
        if (!list) {
            if (stats) atomic_inc64(&stats->trainer_errs, 1);
            continue;
        }

        /* Count and size accumulation with overflow guard */
        size_t count = 0, total = 0;
        for (sample_node_t *p = list; p; p = p->next) {
            count++;
            if (SIZE_MAX - total < p->len) { /* overflow */
                // Drop this batch safely
                for (sample_node_t *q = list; q; ) { sample_node_t *tmp = q->next; free(q->buf); free(q); q = tmp; }
                // Best-effort correction; avoid underflow on concurrent updates
                size_t now_pending = atomic_load_explicit(&ctx->bytes_pending, memory_order_acquire);
                size_t dec = now_pending < pending ? now_pending : pending;
                atomic_fetch_sub_explicit(&ctx->bytes_pending, dec, memory_order_acq_rel);
                continue;
            }
            total += p->len;
        }
        if (count == 0 || total == 0) {
            // Return budget and continue
            size_t now_pending = atomic_load_explicit(&ctx->bytes_pending, memory_order_acquire);
            size_t dec = total <= now_pending ? total : now_pending;
            atomic_fetch_sub_explicit(&ctx->bytes_pending, dec, memory_order_acq_rel);
            // free empty list (shouldn’t happen)
            for (sample_node_t *q = list; q; ) { sample_node_t *tmp = q->next; free(q->buf); free(q); q = tmp; }
            if (stats) atomic_inc64(&stats->trainer_errs, 1);
            continue;
        }

        size_t *sizes = (size_t*)malloc(sizeof(size_t) * count);
        char   *buff  = (char*)malloc(total);
        if (!sizes || !buff) {
            // OOM: drop batch
            for (sample_node_t *q = list; q; ) { sample_node_t *tmp = q->next; free(q->buf); free(q); q = tmp; }
            free(sizes); free(buff);
            size_t now_pending = atomic_load_explicit(&ctx->bytes_pending, memory_order_acquire);
            size_t dec = total <= now_pending ? total : now_pending;
            atomic_fetch_sub_explicit(&ctx->bytes_pending, dec, memory_order_acq_rel);
            if (stats) {
                atomic_inc64(&stats->trainer_errs, 1);
                atomic_set64(&stats->reservoir_bytes, 0);
                atomic_set64(&stats->reservoir_items, 0);
            }
            continue;
        }

        /* Flatten samples */
        sample_node_t *n = list;
        size_t off = 0;
        for (size_t i = 0; i < count; ++i, n = n->next) {
            sizes[i] = n->len;
            memcpy(buff + off, n->buf, n->len);
            off += n->len;
        }

        /* Train dictionary */
        void *dict = calloc(1, max_dict);
        if (!dict) {
            for (sample_node_t *q = list; q; ) { sample_node_t *tmp = q->next; free(q->buf); free(q); q = tmp; }
            free(buff); free(sizes);
            size_t now_pending = atomic_load_explicit(&ctx->bytes_pending, memory_order_acquire);
            size_t dec = total <= now_pending ? total : now_pending;
            atomic_fetch_sub_explicit(&ctx->bytes_pending, dec, memory_order_acq_rel);
            atomic_inc64(&stats->trainer_errs, 1);
            continue;
        }
        size_t dict_sz = train_dictionary(dict, max_dict, buff, sizes, count);

        if (ZSTD_isError(dict_sz)) {
            if (settings.verbose > 1) {
                log_rate_limited(10ULL * 1000000ULL,
                    "mcz-dict: TRAIN ERROR %s (samples=%zu, bytes=%zu)\n",
                    ZSTD_getErrorName(dict_sz), count, total);
            }
            if (stats) atomic_inc64(&stats->trainer_errs, 1);
        } else if (dict_sz < 1024) {
            if (settings.verbose > 1) {
                log_rate_limited(10ULL * 1000000ULL,
                    "mcz-dict: dict too small (%zu B, need ≥1 KiB)\n", dict_sz);
            }
            if (stats) atomic_inc64(&stats->trainer_errs, 1);
        } else {
            if (settings.verbose > 1) {
                log_rate_limited(1000000ULL,
                    "mcz-dict: new dict (%zu B) built from %zu samples\n", dict_sz, count);
            }

            /* Persist dict + manifest (global namespace) */
            char *err = NULL;
            time_t created = time(NULL);  /* single timestamp */
            int rc = mcz_save_dictionary_and_manifest(
                         ctx->cfg->dict_dir,
                         dict, dict_sz,
                         NULL, 0,
                         ctx->cfg->zstd_level,
                         NULL,
                         created,
                         0,
                         NULL, /* out_meta not needed here */
                         &err);
            if (rc == 0) {
                (void)mcz_reload_dictionaries();
                success = true;
            } else {
                fprintf(stderr, "save failed: %s\n", err ? err : "unknown error");
                free(err);
                if (stats) atomic_inc64(&stats->trainer_errs, 1);
            }
        }

        /* Return consumed bytes exactly once (guard underflow on races) */
        size_t now_pending = atomic_load_explicit(&ctx->bytes_pending, memory_order_acquire);
        size_t dec = total <= now_pending ? total : now_pending;
        atomic_fetch_sub_explicit(&ctx->bytes_pending, dec, memory_order_acq_rel);

        /* Free temps and list */
        free(dict);
        for (sample_node_t *q = list; q; ) { sample_node_t *tmp = q->next; free(q->buf); free(q); q = tmp; }
        free(buff);
        free(sizes);
        uint64_t now = (uint64_t)time(NULL);
        if (stats) {
            atomic_set64(&stats->reservoir_bytes, 0);
            atomic_set64(&stats->reservoir_items, 0);
            atomic_set64(&stats->trainer_ms_last, now * 1000);
        }
        if (success) {
            set_training_active(false);               /* stop sampling until EWMA triggers again */
            mcz_eff_mark_retrained(now);
        }
    }

    /* never reached */
}

static int mcz_start_trainer(mcz_ctx_t *ctx){
    if (!ctx)
        return -ENOMEM;
    if (!ctx->cfg->enable_comp){
        return 0;
    }
    if (ctx->cfg->enable_training && ctx->cfg->enable_dict){
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&ctx->trainer_tid, &attr, trainer_main, ctx);
        pthread_attr_destroy(&attr);
        if (settings.verbose > 1) {
            log_rate_limited(1000000ULL,
                             "mcz-dict: trainer thread started (max_dict=%zu B)\n", ctx->cfg->dict_size);
        }
    }
    return 0;
}

/* ---------- public init / destroy ----------------------------------- */
int mcz_init(void) {
    mcz_init_default_config();
    mcz_config_sanity_check();
    attach_cfg();

    mcz_ctx_t *ctx = mcz_ctx_mut();
    if (!ctx)
        return -ENOMEM;
    /* override from global settings*/
    if (settings.disable_comp){
        ctx->cfg->enable_comp = false;
        return 0;
    }
    if (!ctx->cfg->enable_comp){
        return 0;
    }
    mcz_cfg_t *cfg = ctx->cfg;
    /* 1. atomic pointers / counters */
    atomic_init(&ctx->samples_head, NULL); /* empty list            */
    atomic_init(&ctx->bytes_pending, 0);
    ctx->trainer_tid = (pthread_t ) { 0 }; /* will be set by trainer thread */

    /* ---------------- init statistics module ----------------------------*/
    mcz_stats_registry_global_init(0);

    if (!cfg->enable_dict) {
        return 0;
    }

    /* try external dictionary first */
    mcz_load_dicts();

    mcz_train_cfg_t ecfg = {
        .enable_training = true,
        .retraining_interval_s = ctx->cfg->retraining_interval_s,
        .min_training_size = ctx->cfg->min_training_size,
        .ewma_alpha = ctx->cfg->ewma_alpha,
        .retrain_drop = ctx->cfg->retrain_drop
    };

    mcz_eff_configure(&ecfg);                 /* single-threaded init */
    mcz_eff_init((uint64_t)time(NULL));

    /* ---------------- init retired dictionaries pool --------------------- */
    mcz_dict_pool_init();
    /* ---------------- spawn background trainer --------------------------- */
    mcz_start_trainer(ctx);
    /* ---------------- spawn background garbage collector ------------------ */
    mcz_gc_start(ctx);
    if (settings.verbose > 1) {
        log_rate_limited(0ULL,
                         "mcz: GC thread started\n");
    }
    /* ---------------  initialize sampler subsystem --------------------------*/
    mcz_sampler_init(cfg->spool_dir, cfg->sample_p, cfg->sample_window_duration, cfg->spool_max_bytes);
    return 0;
}

void mcz_destroy(void) {
    mcz_ctx_t *ctx = mcz_ctx_mut();

    /* note: trainer thread is detached and loops forever; in production
     we may add a stop flag + join, or just let process exit */

    /* free thread-local caches for the calling thread . TODO: other threads?*/
    if (tls.scratch) {
        free(tls.scratch);
        tls.scratch = NULL;
        tls.cap = 0;
    }
    if (tls.cctx) {
        ZSTD_freeCCtx(tls.cctx);
        tls.cctx = NULL;
    }
    if (tls.dctx) {
        ZSTD_freeDCtx(tls.dctx);
        tls.dctx = NULL;
    }
    mcz_stats_registry_global_destroy();
    mcz_dict_pool_shutdown();
    mcz_gc_stop(ctx);

    free(ctx);

}

static void sample_for_training(const void *src, size_t len) {
    mcz_ctx_t *ctx = mcz_ctx_mut();
    /* skip very large and very small items */
    if (len >ctx->cfg->max_comp_size || len < ctx->cfg->min_comp_size)
        return;
    if (!is_training_active()) /* skip if training is not active */
        return;
    const mcz_table_t* tab = (const mcz_table_t*) atomic_load_explicit(&ctx->dict_table, memory_order_acquire);
    bool empty_state = !mcz_has_default_dict(tab);
    double p = empty_state? 1.0: ctx->cfg->sample_p;

    // Suppose p is in [0,1]. Represent it as fixed-point threshold:
    uint32_t threshold = (uint32_t)((double)UINT32_MAX * p);

    if (fast_rand32() > threshold) {
        return;
    }

    if (is_likely_incompressible((const uint8_t *) src, len)){
        return;
    }

    /* ---- back-pressure: stop once corpus ≥ min_train_bytes ---------- */
    size_t limit =
            ctx->cfg->min_training_size ?
                    ctx->cfg->min_training_size : ctx->cfg->dict_size * 100; /* 100× rule */

    if (atomic_load_explicit(&ctx->bytes_pending, memory_order_relaxed)
            >= limit)
        return; /* trainer hasn’t processed yet     */

    /* ---- allocate sample node -------------------------------------- */
    sample_node_t *node = malloc(sizeof(sample_node_t));
    if (!node)
        return;
    node->buf = malloc(len);
    if (!node->buf) {
        free(node);
        return;
    }
    memcpy(node->buf, src, len);
    node->len = len;

    /* ---- lock-free push into MPSC list ------------------------------ */
    sample_node_t *old_head;
    do {
        old_head = atomic_load_explicit(&ctx->samples_head,
                memory_order_acquire);
        node->next = old_head;
    } while (!atomic_compare_exchange_weak_explicit(&ctx->samples_head,
            &old_head, node, memory_order_release, memory_order_relaxed));

    atomic_fetch_add_explicit(&ctx->bytes_pending, len, memory_order_relaxed);
    /* update statistics for "default" namespace*/
    mcz_stats_atomic_t * stats = mcz_stats_lookup_by_ns("default", 7);
    if(stats) {
        atomic_inc64(&stats->reservoir_bytes, len);
        atomic_inc64(&stats->reservoir_items, 1);
    }
}

void mcz_sample(const void *key, size_t klen, const void* value, size_t vlen) {
    sample_for_training(value, vlen);
    mcz_sampler_maybe_record(key, klen, value, vlen);
}


static inline const ZSTD_DDict*
get_ddict_by_id(uint16_t id)
{
    const mcz_table_t *table = mcz_current_table();
    if(!table) return NULL;
    const mcz_dict_meta_t *meta = mcz_lookup_by_id(table, id);
    if(!meta) return NULL;
    return meta->ddict;
}

static inline const mcz_dict_meta_t *
get_meta_by_key(const char *key, size_t klen) {
    const mcz_table_t *table = mcz_current_table();
    if(!table) return NULL;
    const mcz_dict_meta_t *meta = mcz_pick_dict(table, key, klen);
    return meta;
}

ssize_t inline mcz_orig_size(const void *src, size_t comp_size){
    return ZSTD_getFrameContentSize(src, comp_size);
}

static inline unsigned long long cur_tid(void) {
    return (unsigned long long)(uintptr_t)pthread_self();
}

/*
 * Find namespace for a key.
 *
 * key      : pointer to key bytes
 * klen     : length of the key
 * nspaces  : number of namespace strings
 * spaces   : array of namespace strings (prefixes)
 *
 * Returns: pointer to namespace string (from spaces[]) or NULL if no match.
 */
const char *
mcz_match_namespace(const char *key, size_t klen,
                    const char **spaces, size_t nspaces)
{
    if (!key || !spaces) return NULL;

    const char *best = NULL;
    size_t best_len = 0;

    for (size_t i = 0; i < nspaces; i++) {
        const char *ns = spaces[i];
        if (!ns) continue;

        size_t nlen = strlen(ns);
        if (nlen > klen) continue;  // can't match, key shorter than prefix

        if (memcmp(key, ns, nlen) == 0) {
            // longest-match wins
            if (nlen > best_len) {
                best = ns;
                best_len = nlen;
            }
        }
    }
    return best;
}

bool mcz_dict_exists(uint16_t id) {
    const mcz_table_t *table = mcz_current_table();
    if(!table) return NULL;
    const mcz_dict_meta_t *meta = mcz_lookup_by_id(table, id);
    return meta? true: false;
}

void mcz_report_dict_miss_err(const char *key, size_t klen) {
    mcz_ctx_t *ctx = mcz_ctx_mut();          /* global-static instance */
    if (!ctx->cfg->enable_comp){
        return;
    }
    mcz_stats_atomic_t * stats = mcz_stats_lookup_by_key((const char *) key, klen);
    if(stats) {
        atomic_inc64(&stats->dict_miss_errs, 1);
    }
}

void mcz_report_decomp_err(const char *key, size_t klen) {
    mcz_ctx_t *ctx = mcz_ctx_mut();          /* global-static instance */
    if (!ctx->cfg->enable_comp){
        return;
    }
    mcz_stats_atomic_t * stats = mcz_stats_lookup_by_key((const char *) key, klen);
    if(stats) {
        atomic_inc64(&stats->decompress_errs, 1);
    }
}

/* -----------------------------------------------------------------
 * Compress an value.
 *  • On success: returns compressed size (≥0) and sets *dict_id_out.
 *  • On error  : returns negative errno / ZSTD error; *dict_id_out == 0.
 * ----------------------------------------------------------------*/
ssize_t mcz_maybe_compress(const void *src, size_t src_sz, const void *key, size_t key_sz,
                    void **dst, uint16_t *dict_id_out)
{
    mcz_ctx_t *ctx = mcz_ctx_mut();          /* global-static instance */
    if (!ctx->cfg->enable_comp){
        return 0;
    }
    /* 0.  sanity checks ------------------------------------------ */
    if (!ctx || !src || src_sz == 0 || !dst || !dict_id_out)
        return -EINVAL;
    /* Statistics */
    mcz_stats_atomic_t * stats = mcz_stats_lookup_by_key((const char *) key, key_sz);
    atomic_inc64(&stats->writes_total, 1);
    atomic_inc64(&stats->bytes_raw_total, src_sz);

    if (ctx->cfg->min_comp_size && src_sz < ctx->cfg->min_comp_size){
        atomic_inc64(&stats->skipped_comp_min_size, 1);
        return 0;
    }
    if (ctx->cfg->max_comp_size && src_sz > ctx->cfg->max_comp_size) {
        atomic_inc64(&stats->skipped_comp_max_size, 1);
        return 0;                              /* bypass */
    }

    /* 1.  choose dictionary -------------------------------------- */
    const mcz_dict_meta_t *meta = get_meta_by_key(key, key_sz);
    const ZSTD_CDict *cd = meta? meta->cdict: NULL;
    uint16_t did = meta? meta->id:0;

    /* 2.  prepare TLS scratch ------------------------------------ */
    size_t bound = ZSTD_compressBound(src_sz);
    tls_ensure(bound);                         /* ensure scratch ≥ bound */
    void *dst_buf = tls.scratch;

    /* 3.  compress ----------------------------------------------- */
    size_t csz = cd
        ? ZSTD_compress_usingCDict(tls.cctx, dst_buf, bound,
                                   src, src_sz, cd)
        : ZSTD_compressCCtx      (tls.cctx, dst_buf, bound,
                                   src, src_sz, ctx->cfg->zstd_level);
    if (ZSTD_isError(csz)) {
        atomic_inc64(&stats->compress_errs, 1);
        return -(ssize_t)ZSTD_getErrorCode(csz);
    }
    /* 4. report 'eff' statistics - only for "default" namespace*/
    int rc;
    bool res;
    rc = mcz_stats_is_default(stats, &res);
    if(rc == 0 && res) {
        mcz_eff_on_observation(src_sz, csz);
    }
    /* 5.  ratio check – skip if no benefit ----------------------- */
    if (csz >= src_sz) {
        atomic_inc64(&stats->skipped_comp_incomp, 1);
        return 0;
    }
    atomic_inc64(&stats->bytes_cmp_total, csz);

    /* 6.  success ------------------------------------------------ */
    *dst         = dst_buf;          /* valid until same thread calls tls_ensure() again */
    *dict_id_out = did;              /* 0 = no dictionary             */
    return (ssize_t)csz;
}
/* ------------------------------------------------------------------ */
/* Decompress a value into an iovec array.                             *
 *  - src/src_sz : compressed buffer                                   *
 *  - dst/dst_cnt: scatter-gather destination                          *
 *  - dict_id    : 0 = no dict, ≥1 = dictionary selector               *
 * Returns:                                                            *
 *    ≥0  decompressed bytes                                           *
 *   < 0  negative errno / ZSTD error code                             */
/* ------------------------------------------------------------------ */
ssize_t mcz_decompress(const void *src,
        size_t src_sz, void *dst, size_t dst_sz, uint16_t dict_id) {
    mcz_ctx_t *ctx = mcz_ctx_mut();
    /* 0) sanity checks ----------------------------------------------- */
    if (!ctx || !src || src_sz == 0 || !dst || dst_sz <= 0)
        return -EINVAL; /* invalid arguments */
    /* 1) compute output capacity ---------------------------------- */
    /* 2) pick decompression path ---------------------------------- */
    /* We call this function to init TLS contexts */
    tls_ensure(0);
    size_t out_sz;
    if (dict_id == 0) {
        out_sz = ZSTD_decompressDCtx(tls.dctx, dst, dst_sz, src,
                src_sz);
    } else {
        const ZSTD_DDict *dict = get_ddict_by_id(dict_id);
        if (!dict)
            return -EINVAL; /* unknown dictionary ID     */

        out_sz = ZSTD_decompress_usingDDict(tls.dctx, dst, dst_sz, src,
                src_sz, dict);
    }

    if (ZSTD_isError(out_sz))
        return -(ssize_t)ZSTD_getErrorCode(out_sz);   /* correct sign */

    if (out_sz > dst_sz)
        return -EOVERFLOW;                            /* caller too small */

    return (ssize_t) out_sz;
}



/* Return values
 *   >0  : decompressed length
 *    0  : either ITEM_ZSTD flag not set  *or*  item is chunked
 *   <0  : negative errno / ZSTD error code
 */
ssize_t mcz_maybe_decompress(const item *it, mc_resp    *resp) {
    mcz_ctx_t *ctx = mcz_ctx_mut();
    if (!ctx || !it || !resp){

        fprintf(stderr, "[mcz] decompress: bad args (ctx=%p it=%p resp=%p)\n",
               (void*)ctx, (void*)it, (void*)resp);
        return -EINVAL; /* invalid arguments */
    }

    /* Statistics */
    mcz_stats_atomic_t * stats = mcz_stats_lookup_by_key((const char *) ITEM_key(it), it->nkey);
    if(stats) atomic_inc64(&stats->reads_total, 1);

    /* 1. Skip if not compressed or chunked ------------------------ */
     if (!(it->it_flags & ITEM_ZSTD) || (it->it_flags & ITEM_CHUNKED))
         return 0;                         /* treat as plain payload */

     /* 2. Dictionary lookup --------------------------------------- */
     uint16_t  did = ITEM_get_dictid(it);
     const ZSTD_DDict *dd = get_ddict_by_id(did);
    if (!dd && did > 0){
        fprintf(stderr, "[mcz] decompress: unknown dict id %u\n", did);
        if(stats) atomic_inc64(&stats->dict_miss_errs, 1);
        //TODO: item must be deleted upstream
        return -EINVAL;                  /* unknown dict id */
    }

    /* 3. Prepare destination buffer ------------------------------ */
    const void *src     = ITEM_data(it);
    size_t      compLen = it->nbytes;

    size_t expect = ZSTD_getFrameContentSize(src, compLen);
    if (expect == ZSTD_CONTENTSIZE_ERROR){
        fprintf(stderr, "[mcz] decompress: corrupt frame (tid=%llu, id=%u, compLen=%zu, start=%llu)\n",
               cur_tid(), did, compLen, *(uint64_t *)src);
        if(stats) atomic_inc64(&stats->decompress_errs, 1);
        return -EINVAL;
    }
    if (expect == ZSTD_CONTENTSIZE_UNKNOWN)
        expect = compLen * 4u;           /* pessimistic */

    void *dst = malloc(expect);
    if (!dst){
        fprintf(stderr,"[mcz] decompress: malloc(%zu) failed: %s\n",
                expect, strerror(errno));
        if(stats) atomic_inc64(&stats->decompress_errs, 1);

        return -ENOMEM;
    }

    /* 4. Decompress ---------------------------------------------- */
    ssize_t dec = mcz_decompress(src, compLen, dst, expect, did);

    if (dec < 0) {                       /* ZSTD error */
        fprintf(stderr, "[mcz decompress: mcz_decompress() -> %zd (id=%u)\n",
                dec, did);
        free(dst);
        if(stats) atomic_inc64(&stats->decompress_errs, 1);
        return dec;
    }

    /* 5. Hand buffer to network layer ---------------------------- */
    resp->write_and_free = dst;
    return dec;                          /* decompressed bytes */
}


/* ---- Publish / current (copy-on-write) ---- */

static void mcz_publish_table(mcz_table_t *tab) {
    mcz_ctx_t *ctx = mcz_ctx_mut();

    /* bump generation from current */
    mcz_table_t *old = (mcz_table_t*)atomic_load_explicit(&ctx->dict_table, memory_order_acquire);
    tab->gen = old ? (old->gen + 1) : 1;
    atomic_store_explicit(&ctx->dict_table, (uintptr_t)tab, memory_order_release);
    /* enqueue retired table to GC*/
    if (old) mcz_gc_enqueue_retired(ctx, old);
}

/* ---- Coordinated reload (no pause-the-world) ---- */
int mcz_reload_dictionaries(void)
{
    mcz_ctx_t *ctx = mcz_ctx_mut();
    const char *dir = ctx->cfg->dict_dir;
    char *err = NULL;
    mcz_table_t *newtab = mcz_scan_dict_dir(dir, ctx->cfg->dict_retain_max,
                                         ctx->cfg->gc_quarantine_period, ctx->cfg->zstd_level, &err);
    if (err != NULL){
        fprintf(stderr, "reload dictionaries failed: %s\n", err ? err : "unknown error");
        free(err);
        return -ENOENT;
    }
    mcz_publish_table(newtab);
    return 0;
}

static inline int is_default_ns(const char *ns, size_t ns_sz) {
    static const char defname[] = "default";
    size_t def_sz = sizeof(defname) - 1;
    return (ns && ns_sz == def_sz && memcmp(ns, defname, def_sz) == 0);
}

/* Fill per-namespace metadata; only if ns == "default" add ewma/baseline/etc */
static int
prefill_stats_snapshot_ns(mcz_stats_snapshot_t *snapshot, const char *ns, size_t ns_sz)
{
    if (!snapshot || !ns) return -EINVAL;

    mcz_ctx_t *ctx = mcz_ctx_mut();
    if (!ctx) return -EFAULT;

    mcz_table_t *tab = (mcz_table_t*)atomic_load_explicit(&ctx->dict_table, memory_order_acquire);
    if (!tab) return -ENOENT;

    /* dict meta for this ns (including "default") */
    const mcz_dict_meta_t *meta = mcz_pick_dict(tab, ns, ns_sz);
    if (!meta) return -ENOENT;

    snapshot->dict_id   = meta->id;
    snapshot->dict_size = meta->dict_size;

    /* total dicts configured for this ns */
    {
        int found = 0;
        for (size_t i = 0; i < tab->nspaces; i++) {
            mcz_ns_entry_t *sp = tab->spaces[i];
            if (!sp || !sp->ndicts || !sp->prefix) continue;

            size_t plen = strlen(sp->prefix);
            if (plen == ns_sz && memcmp(ns, sp->prefix, plen) == 0) {
                snapshot->total_dicts = sp->ndicts;
                found = 1;
                break;
            }
        }
        if (!found) return -ENOENT;
    }

    /* Only for the "default" namespace, add efficiency + mode */
    if (is_default_ns(ns, ns_sz)) {
        snapshot->ewma_m          = mcz_eff_get_ewma();
        snapshot->baseline        = mcz_eff_get_baseline();
        snapshot->last_retrain_ms = mcz_eff_last_train_seconds() * 1000; /* seconds; field name kept */
        snapshot->train_mode      = (uint32_t)ctx->cfg->train_mode;
    }

    return 0;
}


/* If ns == NULL → GLOBAL stats (overall); do NOT fill ewma/baseline/last_train/train_mode here.
   If ns != NULL → namespace stats; fill the four fields only when ns == "default". */
int
mcz_get_stats_snapshot(mcz_stats_snapshot_t *snap, const char *ns, size_t ns_sz)
{
    if (!snap) return -EINVAL;
    memset(snap, 0, sizeof(*snap));

    if (ns == NULL) {
        /* GLOBAL (overall) */
        mcz_stats_atomic_t *g = mcz_stats_global();
        if (!g) return -ENOENT;
        mcz_stats_snapshot_fill(g, snap);
        return 0;
    }

    /* Per-namespace (including "default") */
    {
        int rc = prefill_stats_snapshot_ns(snap, ns, ns_sz);
        if (rc < 0) return rc;

        mcz_stats_atomic_t *st = mcz_stats_lookup_by_ns(ns, ns_sz);
        if (!st) return -ENOENT;

        mcz_stats_snapshot_fill(st, snap);
        return 0;
    }
}

const char **mcz_list_namespaces(size_t *count){
    mcz_ctx_t *ctx = mcz_ctx_mut();
    if (!ctx) return NULL;

    mcz_table_t *table = (mcz_table_t*)atomic_load_explicit(&ctx->dict_table, memory_order_acquire);
    if (!table || table->nspaces == 0) {
        if (count) *count = 0;
        return NULL;
    }
    /* Build temporary view into existing prefixes */
    const char **list = malloc(table->nspaces * sizeof(char *));
    if (!list) {
        if (count) *count = 0;
        return NULL;
    }
    char * def_name = "default";
    for (size_t i = 0; i < table->nspaces; i++) {
        mcz_ns_entry_t *ns = table->spaces[i];
        if (!strcmp(ns->prefix, def_name)){
            continue;
        }
        list[i] = ns ? ns->prefix : "";
    }
    if (count) *count = table->nspaces;

    return list;

}


