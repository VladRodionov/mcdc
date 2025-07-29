#define _GNU_SOURCE
#include "zstd_compression.h"

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

/* ---------- TLS cache per worker thread ----------------------------- */
typedef struct {
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
    void *scratch;
    size_t cap;
} tls_cache_t;

static __thread tls_cache_t tls; /* zero-initialised */

/* ---------- helper macros ------------------------------------------- */
#define CDICT(ctx) ((ZSTD_CDict*)atomic_load_explicit(&(ctx)->cdict, memory_order_acquire))
#define DDICT(ctx) ((ZSTD_DDict*)atomic_load_explicit(&(ctx)->ddict, memory_order_acquire))

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

static zstd_stats_t zstd_stats = { 0 };

/* Optional helper you can expose via “stats zstd” command */
void zstd_get_stats(zstd_stats_t *out) {
    *out = zstd_stats;
}

/* ----------------------------------------------------------------------
 * log_rate_limited()
 *   Prints to stderr at most once every `interval_us` micro-seconds.
 *   Uses a static timestamp; thread-safe under POSIX (atomic exchange).
 * -------------------------------------------------------------------- */

static uint64_t now_usec(void) /* monotonic wall-clock */
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

static void log_rate_limited(uint64_t interval_us, const char *fmt, ...) {

    /* time of last log */
    static _Atomic(uint64_t) last_ts = 0;

    uint64_t now = now_usec();
    uint64_t prev = atomic_load_explicit(&last_ts, memory_order_relaxed);

    if (now - prev < interval_us)
        return; /* still within quiet window */

    /* attempt to claim the slot */
    if (!atomic_compare_exchange_strong_explicit(&last_ts, &prev, now,
            memory_order_acq_rel, memory_order_relaxed))
        return; /* another thread logged */

    /* we won the race → emit message */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------------
 * loadDictionary()
 *   If ctx->cfg.dict_path is non-NULL, reads the file, builds CDict/DDict,
 *   marks ctx->dict_ready = true, and returns 0.
 *   If dict_path == NULL it returns 1 (nothing loaded, continue live training).
 *   On I/O/alloc/ZSTD errors returns a negative errno.
 * ------------------------------------------------------------------- */
static int zstd_load_dict(zstd_ctx_t *ctx) {
    if (!ctx->cfg.dict_path)
        return 1; /* nothing to load, continue live training */

    FILE *f = fopen(ctx->cfg.dict_path, "rb");
    if (!f)
        return -errno; /* I/O error */

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        return 1; /* zero-length file: ignore silently */
    }

    void *buf = malloc(file_size);
    if (!buf) {
        fclose(f);
        return -ENOMEM;
    }

    size_t off = 0;
    while (off < (size_t) file_size) {
        size_t n = fread((char*) buf + off, 1, (size_t) file_size - off, f);
        if (n == 0) { /* I/O error or EOF too early */
            free(buf);
            fclose(f);
            return -EIO;
        }
        off += n;
    }
    fclose(f);

    ZSTD_CDict *cd = ZSTD_createCDict(buf, file_size, ctx->cfg.level);
    ZSTD_DDict *dd = ZSTD_createDDict(buf, file_size);

    if (!cd || !dd) {
        free(buf);
        if (cd)
            ZSTD_freeCDict(cd);
        if (dd)
            ZSTD_freeDDict(dd);
        return -ENOMEM; /* alloc failed */
    }

    atomic_store_explicit(&ctx->cdict,
            (uintptr_t) ZSTD_createCDict(buf, file_size, ctx->cfg.level),
            memory_order_release);
    atomic_store_explicit(&ctx->ddict,
            (uintptr_t) ZSTD_createDDict(buf, file_size), memory_order_release);
    free(buf);
    /* load file exactly as before ... */
    atomic_store(&ctx->dict_ready, true); /* disable training */
    return 0;
}

static int zstd_save_dict(const void *dict, size_t dict_size, zstd_ctx_t *ctx,
bool overwrite) {
    const char *path = ctx->cfg.dict_path;
    if (!dict || !dict_size || !path)
        return -EINVAL;

    int flags = O_WRONLY | O_CREAT | (overwrite ? 0 : O_EXCL);
    int fd = open(path, flags, 0644);
    if (fd == -1)
        return -errno; /* ENOENT, EEXIST, etc. */

    const char *p = dict;
    size_t n = dict_size;
    while (n) { /* robust write loop    */
        ssize_t w = write(fd, p, n);
        if (w <= 0) { /* error or short write */
            int e = errno;
            close(fd);
            return -e ? -e : -EIO;
        }
        p += w;
        n -= (size_t) w;
    }
    close(fd);
    return 0;
}

/* ---------- trainer thread ------------------------------------------ */
static void* trainer_main(void *arg) {
    zstd_ctx_t *ctx = arg;
    size_t max_dict = ctx->cfg.max_dict ? ctx->cfg.max_dict : 110 * 1024;
    size_t train_threshold =
            ctx->cfg.min_train_bytes ?
                    ctx->cfg.min_train_bytes : max_dict * 100; /* 100× rule */

    while (1) {
        usleep(100000); /* 100 ms */
        bool success = false;
        if (atomic_load_explicit(&ctx->bytes_pending, memory_order_acquire)
                < train_threshold)
            continue;
        /* Atomically take ownership of the whole list */
        sample_node_t *list = atomic_exchange_explicit(&ctx->samples_head,
        NULL, memory_order_acq_rel);
        if (!list)
            continue; /* rare race */
        /* Walk list, count nodes & bytes, fill arrays */
        size_t count = 0, total = 0;
        for (sample_node_t *n = list; n; n = n->next) {
            count++;
            total += n->len;
        }
        size_t *sizes = malloc(sizeof(size_t) * count);
        void **buffs = malloc(sizeof(void*) * count);
        sample_node_t *n = list;
        for (size_t i = 0; i < count; ++i, n = n->next) {
            sizes[i] = n->len;
            buffs[i] = n->buf;
        }
        /* train dictionary */
        void *dict = malloc(max_dict);
        size_t dict_sz = ZDICT_trainFromBuffer(dict, max_dict, buffs, sizes,
                count);

        if (ZSTD_isError(dict_sz)) { /* hard failure      */
            log_rate_limited(10 * 1000000ULL, /* 10 s */
            "zstd-dict: TRAIN ERROR %s (samples=%zu, bytes=%zu)\n",
                    ZSTD_getErrorName(dict_sz), count, total);
            atomic_fetch_add_explicit(&zstd_stats.train_err, 1,
                    memory_order_relaxed);
        } else if (dict_sz < 1024) { /* too small         */
            log_rate_limited(10 * 1000000ULL,
                    "zstd-dict: dict too small (%zu B, need ≥1 KiB)\n",
                    dict_sz);
            atomic_fetch_add_explicit(&zstd_stats.train_small, 1,
                    memory_order_relaxed);
        } else {
            ZSTD_CDict *nc = ZSTD_createCDict(dict, dict_sz,
                    ctx->cfg.level ? ctx->cfg.level : 1);
            ZSTD_DDict *nd = ZSTD_createDDict(dict, dict_sz);

            if (nc && nd) {
                /* OPTIONAL: save the raw dictionary bytes for future cold-start */
                if (ctx->cfg.dict_path) { /* reuse same path or another config field */
                    int rc = zstd_save_dict(dict, dict_sz, ctx, /* or other path */
                    /* overwrite = */true);
                    if (rc)
                        log_rate_limited(10 * 1000000ULL,
                                "zstd-dict: could not save dict (%s): %s\n",
                                ctx->cfg.dict_path, strerror(-rc));
                }
                /* atomically swap in new dicts */
                uintptr_t oldc = atomic_exchange_explicit(&ctx->cdict,
                        (uintptr_t) nc, memory_order_acq_rel);
                uintptr_t oldd = atomic_exchange_explicit(&ctx->ddict,
                        (uintptr_t) nd, memory_order_acq_rel);
                if (oldc)
                    ZSTD_freeCDict((ZSTD_CDict*) oldc);
                if (oldd)
                    ZSTD_freeDDict((ZSTD_DDict*) oldd);
                /* keep `dict` alive: owned by nc/nd */
                /* SUCCESS → publish */
                atomic_store(&ctx->dict_ready, true);
                atomic_fetch_add_explicit(&zstd_stats.train_ok, 1,
                        memory_order_relaxed);
                success = true;
            } else {
                log_rate_limited(10 * 1000000ULL,
                        "zstd-dict: CDict/DDict alloc failed\n");
                atomic_fetch_add_explicit(&zstd_stats.train_err, 1,
                        memory_order_relaxed);
                if (nc)
                    ZSTD_freeCDict(nc);
                if (nd)
                    ZSTD_freeDDict(nd);
            }
        } // What to do if training fails?

        free(dict);
        /* Free nodes & sample buffers */
        n = list;
        while (n) {
            sample_node_t *tmp = n->next;
            free(n->buf);
            free(n);
            n = tmp;
        }
        free(buffs);
        free(sizes);
        if (success) {
            break;
        } else {
            atomic_fetch_sub_explicit(&ctx->bytes_pending, total,
                    memory_order_acq_rel);
        }
    }
    return NULL; /* never reached */
}

static inline bool dict_is_ready(zstd_ctx_t *c) {
    /* any cheap acquire fence is fine; on x86 it's a compiler barrier */
    /* Do we really need this? - this will trash performance*/
    atomic_thread_fence(memory_order_acquire);
    return c->dict_ready;
}

/* ---------- public init / destroy ----------------------------------- */
int zstd_init(zstd_ctx_t **out, const zstd_cfg_t *cfg) {
    zstd_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -ENOMEM;
    /* 1. configuration */
    ctx->cfg = cfg ? *cfg : (zstd_cfg_t ) { .level = 1, .max_dict = 110 * 1024,
                             .min_train_bytes = 110 * 1024 * 100, .dict_path =
                                     NULL };
    /* 2. atomic pointers / counters */
    atomic_init(&ctx->samples_head, NULL); /* empty list            */
    atomic_init(&ctx->bytes_pending, 0);
    atomic_init(&ctx->cdict, (uintptr_t) NULL);
    atomic_init(&ctx->ddict, (uintptr_t) NULL);
    atomic_init(&ctx->dict_ready, false);
    ctx->trainer_tid = (pthread_t ) { 0 }; /* will be set by trainer thread */

    /* try external dictionary first */
    int rc = zstd_load_dict(ctx);
    if (rc < 0) {
        free(ctx);
        return rc;
    }
    if (rc == 0) {
        ctx->trainer_tid = pthread_self(); /* dummy thread ID *//* dictionary loaded → done     */
        *out = ctx;
        return 0;
    }

    /* ---------------- spawn background trainer --------------------------- */

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&ctx->trainer_tid, &attr, trainer_main, ctx);
    pthread_attr_destroy(&attr);
    log_rate_limited(1000000ULL, /* 1 s */
    "zstd-dict: trainer thread started (max_dict=%zu B)\n", ctx->cfg.max_dict);

    *out = ctx;
    return 0;
}

void zstd_destroy(zstd_ctx_t *ctx) {
    /* note: trainer thread is detached and loops forever; in production
     you may add a stop flag + join, or just let process exit */
    ZSTD_CDict *cd = CDICT(ctx);
    ZSTD_DDict *dd = DDICT(ctx);
    if (cd)
        ZSTD_freeCDict(cd);
    if (dd)
        ZSTD_freeDDict(dd);

    free(ctx);

    /* free thread-local caches for the calling thread */
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
}

void zstd_sample(zstd_ctx_t *ctx, const void *src, size_t len) {
    if (len > 16 * 1024) /* skip very large items            */
        return;
    if (dict_is_ready(ctx)) /* skip if dictionary is ready */
        return;
    /* ---- back-pressure: stop once corpus ≥ min_train_bytes ---------- */
    size_t limit =
            ctx->cfg.min_train_bytes ?
                    ctx->cfg.min_train_bytes : ctx->cfg.max_dict * 100; /* 100× rule */

    if (atomic_load_explicit(&ctx->bytes_pending, memory_order_relaxed)
            >= limit)
        return; /* trainer hasn’t processed yet     */

    /* ---- allocate sample node -------------------------------------- */
    sample_node_t *node = malloc(sizeof(*node));
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
}

/* ---------- compression ---------------------------------------------- */
ssize_t zstd_compress_iov(zstd_ctx_t *ctx, const struct iovec *src, int src_cnt,
        void **dst, size_t *dst_cap) {
    /* 1) compute source size */
    size_t src_sz = 0;
    for (int i = 0; i < src_cnt; ++i)
        src_sz += src[i].iov_len;

    /* 2) copy source into TLS scratch */
    tls_ensure(src_sz); /* scratch ≥ src_sz            */
    char *src_buf = tls.scratch;
    char *p = src_buf;
    for (int i = 0; i < src_cnt; ++i) {
        memcpy(p, src[i].iov_base, src[i].iov_len);
        p += src[i].iov_len;
    }

    /* 3) make sure caller-supplied dst buffer is large enough           */
    size_t out_bound = ZSTD_compressBound(src_sz);
    if (!*dst || *dst_cap < out_bound) {
        void *tmp = realloc(*dst, out_bound);
        if (!tmp)
            return -ENOMEM;
        *dst = tmp;
        *dst_cap = out_bound;
    }
    void *dst_buf = *dst;

    /* 4) compress */
    size_t out_sz;
    if (CDICT(ctx))
        out_sz = ZSTD_compress_usingCDict(tls.cctx, dst_buf, out_bound, src_buf,
                src_sz, CDICT(ctx));
    else
        out_sz = ZSTD_compressCCtx(tls.cctx, dst_buf, out_bound, src_buf,
                src_sz, ctx->cfg.level);

    if (ZSTD_isError(out_sz))
        return -(ssize_t) out_sz;

    /* 5) (optional) shrink dst buffer to actual size                     */
    if (out_sz < *dst_cap) {
        void *tmp = realloc(*dst, out_sz);
        if (tmp) {
            *dst = tmp;
            *dst_cap = out_sz;
        }
    }
    return (ssize_t) out_sz;
}

/* ---------- decompression -------------------------------------------- */
ssize_t zstd_decompress_into_iov(zstd_ctx_t *ctx, const void *src,
        size_t src_sz, const struct iovec *dst, int dst_cnt) {
    size_t out_cap = 0;
    for (int i = 0; i < dst_cnt; ++i)
        out_cap += dst[i].iov_len;

    tls_ensure(out_cap);

    size_t ret;
    if (DDICT(ctx))
        ret = ZSTD_decompress_usingDDict(tls.dctx, tls.scratch, out_cap, src,
                src_sz, DDICT(ctx));
    else
        ret = ZSTD_decompressDCtx(tls.dctx, tls.scratch, out_cap, src, src_sz);

    if (ZSTD_isError(ret))
        return -(ssize_t) ret;
    if (ret != out_cap)
        return -EINVAL;

    /* scatter */
    char *q = tls.scratch;
    for (int i = 0; i < dst_cnt; ++i) {
        memcpy(dst[i].iov_base, q, dst[i].iov_len);
        q += dst[i].iov_len;
    }
    return (ssize_t) ret;
}
