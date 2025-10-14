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
 * mcz_stats.c
 *
 * Implementation of statistics subsystem for Memcarrot (mcz).
 *
 * Responsibilities:
 *   - Implement initialization and update of atomic counters.
 *   - Manage global and per-namespace statistics via a registry.
 *   - Provide lookup and get-or-create functions for namespace stats.
 *   - Support atomic rebuild of namespace tables when manifests change.
 *   - Implement snapshot and reporting functions used by admin commands.
 *
 * Design:
 *   - Counters are updated lock-free with relaxed atomics.
 *   - Readers obtain consistent snapshots without blocking writers.
 *   - Registry rebuild is atomic; old tables are freed after readers drain.
 *   - Reporting is allocation-free in hot path; allocations only occur
 *     during snapshot or admin command output.
 *
 * Naming convention:
 *   - All functions and types prefixed with `mcz_stats_*`.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   // for O_CLOEXEC on glibc + some GNU bits
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L  // for setenv(), unsetenv(), realpath(), etc.
#endif

#include "mcz_stats.h"
#include "mcz_utils.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>


static mcz_stats_registry_t *g_reg = NULL;

static inline uint64_t ld64(_Atomic uint64_t* a){
    return atomic_load_explicit(a, memory_order_relaxed);
}

static inline uint32_t ld32(_Atomic uint32_t* a){
    return atomic_load_explicit(a, memory_order_relaxed);
}

static inline int64_t  ld64s(_Atomic int64_t*  a){
    return atomic_load_explicit(a, memory_order_relaxed);
}

static mcz_ns_table_t *table_new(size_t nbuckets) {
    if (nbuckets == 0) nbuckets = 256;
    mcz_ns_table_t *t = (mcz_ns_table_t *)xzmalloc(sizeof(*t));
    if (!t) return NULL;
    atomic_store_explicit(&t->refcnt, 0, memory_order_relaxed);
    t->nbuckets = nbuckets;
    t->buckets = (mcz_stats_ns_entry_t **)xzmalloc(nbuckets * sizeof(*t->buckets));
    if (!t->buckets) { free(t); return NULL; }
    return t;
}

/* Free table and all entry names; DO NOT free stats blocks here unless told to */
static void table_free(mcz_ns_table_t *t, int free_stats) {
    if (!t) return;
    for (size_t i = 0; i < t->nbuckets; ++i) {
        mcz_stats_ns_entry_t *e = t->buckets[i];
        while (e) {
            mcz_stats_ns_entry_t *n = e->next;
            free((void*)e->name);
            if (free_stats && e->stats) free(e->stats);
            free(e);
            e = n;
        }
    }
    free(t->buckets);
    free(t);
}

static void mcz_stats_init(mcz_stats_atomic_t* s){
    memset(s, 0, sizeof(*s));
}

static inline size_t bucket_idx(size_t nbuckets, const char *ns) {
    return (size_t)(fnv1a64(ns) % nbuckets);
}
/*
 * Initialize stats registry and ensure "default" namespace exists.
 */
static int
mcz_stats_registry_init(mcz_stats_registry_t *r, size_t nbuckets)
{
    if (!r) return -EINVAL;

    mcz_stats_init(&r->global);

    /* build empty table */
    mcz_ns_table_t *t = table_new(nbuckets);
    if (!t) return -ENOMEM;

    /* create "default" namespace entry */

    const char *defname = "default";
    mcz_stats_atomic_t *stats = (mcz_stats_atomic_t *)calloc(1, sizeof(*stats));
    if (!stats) { table_free(t, /*free_stats=*/1); return -ENOMEM; }
    mcz_stats_init(stats);

    size_t b = bucket_idx(t->nbuckets, defname);
    mcz_stats_ns_entry_t *e = (mcz_stats_ns_entry_t *)calloc(1, sizeof(*e));
    if (!e) { table_free(t, /*free_stats=*/1); return -ENOMEM; }

    e->name = strdup(defname);
    e->name_len = strlen(defname); // "default"
    if (!e->name) { free(e); table_free(t, /*free_stats=*/1); return -ENOMEM; }

    e->stats = stats;
    e->next  = t->buckets[b];
    t->buckets[b] = e;

    atomic_store_explicit(&r->default_stats, stats, memory_order_release);
    atomic_store_explicit(&r->only_default, (uint8_t)1, memory_order_release);

    /* publish */
    atomic_store_explicit(&r->cur, t, memory_order_release);
    return 0;
}

static void mcz_stats_registry_destroy(mcz_stats_registry_t *r) {
    if (!r) return;
    mcz_ns_table_t *t = atomic_load_explicit(&r->cur, memory_order_acquire);
    // Wait for outstanding readers on final table (should be zero unless bug)
    while (atomic_load_explicit(&t->refcnt, memory_order_acquire) != 0) { /* spin briefly */ }
    table_free(t, /*free_stats=*/1);
}


/* Sum per-namespace counters into the global block.
   Returns 0 on success, -ENOENT if the registry/table is unavailable. */
static int mcz_stats_sync_global(void)
{
    mcz_stats_registry_t *r = mcz_stats_registry_global();
    if (!r) return -ENOENT;

    mcz_ns_table_t *t = atomic_load_explicit(&r->cur, memory_order_acquire);
    if (!t) return -ENOENT;

    /* Hold a stable snapshot of the table while iterating */
    atomic_fetch_add_explicit(&t->refcnt, 1, memory_order_acq_rel);

    uint64_t bytes_raw_total  = 0;
    uint64_t bytes_cmp_total  = 0;
    uint64_t writes_total     = 0;
    uint64_t reads_total      = 0;

    uint64_t compress_errs    = 0;
    uint64_t decompress_errs  = 0;
    uint64_t dict_miss_errs   = 0;
    uint64_t skipped_comp_min_size = 0;
    uint64_t skipped_comp_max_size = 0;
    uint64_t skipped_comp_incomp   = 0;

    for (size_t i = 0; i < t->nbuckets; ++i) {
        for (mcz_stats_ns_entry_t *e = t->buckets[i]; e; e = e->next) {
            mcz_stats_atomic_t *st = e->stats;
            if (!st) continue;

            bytes_raw_total += atomic_load_explicit(&st->bytes_raw_total,  memory_order_relaxed);
            bytes_cmp_total += atomic_load_explicit(&st->bytes_cmp_total,  memory_order_relaxed);
            writes_total    += atomic_load_explicit(&st->writes_total,  memory_order_relaxed);
            reads_total     += atomic_load_explicit(&st->reads_total,  memory_order_relaxed);

            compress_errs   += atomic_load_explicit(&st->compress_errs,    memory_order_relaxed);
            decompress_errs += atomic_load_explicit(&st->decompress_errs,  memory_order_relaxed);
            dict_miss_errs  += atomic_load_explicit(&st->dict_miss_errs,   memory_order_relaxed);
            skipped_comp_min_size   += atomic_load_explicit(&st->skipped_comp_min_size,    memory_order_relaxed);
            skipped_comp_max_size += atomic_load_explicit(&st->skipped_comp_max_size,  memory_order_relaxed);
            skipped_comp_incomp  += atomic_load_explicit(&st->skipped_comp_incomp,   memory_order_relaxed);
        }
    }
    /* add default */
    mcz_stats_atomic_t *st = atomic_load_explicit(&r->default_stats, memory_order_acquire);
    bytes_raw_total += atomic_load_explicit(&st->bytes_raw_total,  memory_order_relaxed);
    bytes_cmp_total += atomic_load_explicit(&st->bytes_cmp_total,  memory_order_relaxed);
    writes_total    += atomic_load_explicit(&st->writes_total,  memory_order_relaxed);
    reads_total     += atomic_load_explicit(&st->reads_total,  memory_order_relaxed);

    compress_errs   += atomic_load_explicit(&st->compress_errs,    memory_order_relaxed);
    decompress_errs += atomic_load_explicit(&st->decompress_errs,  memory_order_relaxed);
    dict_miss_errs  += atomic_load_explicit(&st->dict_miss_errs,   memory_order_relaxed);
    skipped_comp_min_size   += atomic_load_explicit(&st->skipped_comp_min_size,    memory_order_relaxed);
    skipped_comp_max_size += atomic_load_explicit(&st->skipped_comp_max_size,  memory_order_relaxed);
    skipped_comp_incomp  += atomic_load_explicit(&st->skipped_comp_incomp,   memory_order_relaxed);

    /* Release the snapshot */
    atomic_fetch_sub_explicit(&t->refcnt, 1, memory_order_acq_rel);

    /* Publish totals to the global block */
    atomic_store_explicit(&r->global.bytes_raw_total,  bytes_raw_total,  memory_order_relaxed);
    atomic_store_explicit(&r->global.bytes_cmp_total,  bytes_cmp_total,  memory_order_relaxed);
    atomic_store_explicit(&r->global.writes_total,  writes_total,  memory_order_relaxed);
    atomic_store_explicit(&r->global.reads_total,  reads_total,  memory_order_relaxed);

    atomic_store_explicit(&r->global.compress_errs,    compress_errs,    memory_order_relaxed);
    atomic_store_explicit(&r->global.decompress_errs,  decompress_errs,  memory_order_relaxed);
    atomic_store_explicit(&r->global.dict_miss_errs,  dict_miss_errs,  memory_order_relaxed);

    atomic_store_explicit(&r->global.skipped_comp_min_size,  skipped_comp_min_size,  memory_order_relaxed);
    atomic_store_explicit(&r->global.skipped_comp_max_size,  skipped_comp_max_size,  memory_order_relaxed);
    atomic_store_explicit(&r->global.skipped_comp_incomp,   skipped_comp_incomp,   memory_order_relaxed);

    return 0;
}

/* Public API */

mcz_stats_atomic_t *mcz_stats_global(void) {
    mcz_stats_sync_global();
    mcz_stats_registry_t *r = g_reg;
    return r ? &r->global : NULL;
}

int mcz_stats_registry_global_init(size_t nbuckets)
{
    if (g_reg) return 0;  // already initialized (single thread)
    mcz_stats_registry_t *r = calloc(1, sizeof(*r));
    if (!r) return -1;
    int rc = mcz_stats_registry_init(r, nbuckets);
    if (rc != 0) { free(r); return rc; }
    g_reg = r; // publish before any worker threads start
    return 0;
}

mcz_stats_registry_t *mcz_stats_registry_global(void)
{
    return g_reg; // safe after init, no atomic needed
}

void mcz_stats_registry_global_destroy(void)
{
    if (!g_reg) return;
    mcz_stats_registry_destroy(g_reg);
    free(g_reg);
    g_reg = NULL;
}

void mcz_stats_add_io(mcz_stats_atomic_t* s, uint64_t raw, uint64_t cmp) {
    atomic_fetch_add_explicit(&s->bytes_raw_total, raw, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->bytes_cmp_total, cmp, memory_order_relaxed);
}

void mcz_stats_inc_err(mcz_stats_atomic_t* s, const char* kind){
    if (!kind) return;
    if (kind[0]=='c') atomic_fetch_add(&s->compress_errs, 1);
    else if (kind[0]=='d') atomic_fetch_add(&s->decompress_errs, 1);
    else atomic_fetch_add(&s->dict_miss_errs, 1);
}


void mcz_stats_snapshot_fill(mcz_stats_atomic_t* s,
                             mcz_stats_snapshot_t* o)
{

    o->bytes_raw_total = ld64(&s->bytes_raw_total);
    o->bytes_cmp_total = ld64(&s->bytes_cmp_total);
    o->writes_total = ld64(&s->writes_total);
    o->reads_total = ld64(&s->reads_total);

    o->cr_current = o->bytes_cmp_total? (double)o->bytes_raw_total / (double) o->bytes_cmp_total: 0.0;

    o->retrain_count = ld32(&s->retrain_count);

    o->shadow_samples = ld64(&s->shadow_samples);
    o->shadow_raw_total = ld64(&s->shadow_raw_total);
    o->shadow_saved_bytes = ld64s(&s->shadow_saved_bytes);
    o->promotions = ld32(&s->promotions);
    o->rollbacks  = ld32(&s->rollbacks);

    o->triggers_rise = ld32(&s->triggers_rise);
    o->triggers_drop = ld32(&s->triggers_drop);

    o->trainer_runs = ld64(&s->trainer_runs);
    o->trainer_errs = ld64(&s->trainer_errs);
    o->trainer_ms_last = ld64(&s->trainer_ms_last);

    o->reservoir_bytes = ld64(&s->reservoir_bytes);
    o->reservoir_items = ld64(&s->reservoir_items);

    o->compress_errs = ld64(&s->compress_errs);
    o->decompress_errs = ld64(&s->decompress_errs);
    o->dict_miss_errs = ld64(&s->dict_miss_errs);

    o->skipped_comp_min_size = ld64(&s->skipped_comp_min_size);
    o->skipped_comp_max_size = ld64(&s->skipped_comp_max_size);
    o->skipped_comp_incomp = ld64(&s->skipped_comp_incomp);
}


/* Lookup stats by key (prefix match).
 * If no namespace matches, returns the "default" namespace stats.
 * Returns NULL only if the registry/table is uninitialized and no default exists.
 */
mcz_stats_atomic_t *
mcz_stats_lookup_by_key(const char *key, size_t klen)
{
    const mcz_stats_registry_t *r = g_reg;
    if (!r || !key) return NULL;
    bool only_default = (uint8_t)atomic_load_explicit(&r->only_default, memory_order_acquire) != 0;
    mcz_stats_atomic_t *def = (mcz_stats_atomic_t *) atomic_load_explicit(&r->default_stats, memory_order_acquire);
    if (only_default){
        return def;
    }
    mcz_ns_table_t *t = atomic_load_explicit(&r->cur, memory_order_acquire);
    if (!t) return NULL;

    atomic_fetch_add_explicit(&t->refcnt, 1, memory_order_acq_rel);

    mcz_stats_atomic_t *found = NULL;

    for (size_t i = 0; i < t->nbuckets; ++i) {
        mcz_stats_ns_entry_t *e = t->buckets[i];
        for (; e; e = e->next) {
            const char *ns = e->name;
            if (!ns) continue;
            size_t nlen = e->name_len;
            if (nlen > klen) continue;
            if (key[0] == ns[0] && memcmp(key, ns, nlen) == 0) {
                found = e->stats;
                break;
            }
        }
        if(found) break;
    }
    atomic_fetch_sub_explicit(&t->refcnt, 1, memory_order_acq_rel);
    if (found) {
        return found;
    } else {
        /* Fallback: return "default" namespace stats */
        return def;
    }
 }

mcz_stats_atomic_t *mcz_stats_default(void) {
    return (mcz_stats_atomic_t *) atomic_load_explicit(&g_reg->default_stats, memory_order_acquire);
}

mcz_stats_atomic_t *
mcz_stats_lookup_by_ns(const char *nsp, size_t nsp_sz)
{
    mcz_stats_registry_t *r = g_reg;
    if (!r) return NULL;
    if (!nsp){
        // Global stats
        return &r->global;
    }
    {
        // Check if it is default
        char *def_name = "default";
        if (nsp_sz == strlen(def_name) && strcmp(nsp, def_name) == 0){
            return mcz_stats_default();
        }
    }
    mcz_ns_table_t *t = atomic_load_explicit(&r->cur, memory_order_acquire);
    if (!t) return NULL;

    atomic_fetch_add_explicit(&t->refcnt, 1, memory_order_acq_rel);

    mcz_stats_atomic_t *found = NULL;

    for (size_t i = 0; i < t->nbuckets; ++i) {
        mcz_stats_ns_entry_t *e = t->buckets[i];
        for (; e; e = e->next) {
            const char *ns = e->name;
            if (!ns) continue;
            size_t nlen = strlen(ns);
            if (nlen != nsp_sz) continue;

            if (memcmp(nsp, ns, nlen) == 0) {
                found = e->stats;
                break;
            }
        }
        if(found) break;
    }
    atomic_fetch_sub_explicit(&t->refcnt, 1, memory_order_acq_rel);
    return found;
}


/* Local helper : find stats in the old table by namespace name. */
static mcz_stats_atomic_t *
find_stats(const mcz_ns_table_t *old, const char *name)
{
    size_t b = bucket_idx(old->nbuckets, name);
    const mcz_stats_ns_entry_t *e = old->buckets[b];
    while (e) {
        if (strcmp(e->name, name) == 0) return e->stats;
        e = e->next;
    }
    return NULL;
}

int mcz_stats_is_default(mcz_stats_atomic_t * stats, bool *res){
    mcz_stats_registry_t *r = g_reg;
    if (!r) return -EINVAL;
    mcz_stats_atomic_t * def = (mcz_stats_atomic_t *) atomic_load_explicit(&r->default_stats, memory_order_acquire);
    if(!def) return -EINVAL;
    if(res) *res = stats == def;
    return 0;
}

/* Rebuild the namespace table from a list of names.
   names[] : array of N C strings (namespace names)
   N       : count
   nbuckets_new : hashing fanout for the new table, can be 0
   Returns 0 on success, <0 on error. */
int mcz_stats_rebuild_from_list(const char **names, size_t N, size_t nbuckets_new)
{
    mcz_stats_registry_t *r = g_reg;
    if (!r) return -EINVAL;

    /* Snapshot current table */
    mcz_ns_table_t *old = atomic_load_explicit(&r->cur, memory_order_acquire);
    if (!old) return -EINVAL;

    /* Build a new immutable table */
    mcz_ns_table_t *neu = table_new(nbuckets_new ? nbuckets_new : old->nbuckets);
    if (!neu) return -ENOMEM;

    /* Populate new table entries, reusing old stats when possible */

    for (size_t i = 0; i < N; ++i) {
        const char *name = names[i];
        mcz_stats_ns_entry_t *e;
        mcz_stats_atomic_t *stats;
        size_t b;

        if (!name || !*name) continue;

        stats = find_stats(old, name);
        if (!stats) {
            /* New namespace: allocate fresh stats block */
            stats = (mcz_stats_atomic_t *)calloc(1, sizeof(*stats));
            if (!stats) { table_free(neu, /*free_stats=*/1); return -ENOMEM; }
            mcz_stats_init(stats);
        }

        /* Insert entry into new table */
        b = bucket_idx(neu->nbuckets, name);
        e = (mcz_stats_ns_entry_t *)calloc(1, sizeof(*e));
        if (!e) { table_free(neu, /*free_stats=*/1); return -ENOMEM; }

        e->name = strdup(name);
        if (!e->name) { free(e); table_free(neu, /*free_stats=*/1); return -ENOMEM; }

        e->stats = stats;
        e->name_len = strlen(name);
        e->next  = neu->buckets[b];
        neu->buckets[b] = e;
    }
    atomic_store_explicit(&r->only_default, (uint8_t) (N == 0), memory_order_release);
    /* Publish new table: readers that start after this see 'neu' */
    atomic_store_explicit(&r->cur, neu, memory_order_release);

    /* Wait for readers on the old snapshot to drain */
    struct timespec ts = {0, 1000000};  // 1 ms
    while (atomic_load_explicit(&old->refcnt, memory_order_acquire) != 0) {
        /* Or sleep a little if you prefer */
        nanosleep(&ts, NULL);
    }

    /*
       Do NOT free stats for namespaces that disappeared.
       We intentionally allow tiny leaks to keep lookups lock-free & simple. */

    /* Free the old table structure (names/entries/array); keep stats intact. */
    table_free(old, /*free_stats=*/0);
    return 0;
}

void mcz_stats_snapshot_dump(const mcz_stats_snapshot_t *s, const char *ns)
{
    if (!s) {
        printf("(null snapshot)\n");
        return;
    }

    printf("=== MCZ Stats Snapshot [%s] ===\n", ns ? ns : "global");

    printf("ewma_m           : %.6f\n", s->ewma_m);
    printf("baseline         : %.6f\n", s->baseline);
    printf("cr_current       : %.6f\n", s->cr_current);

    printf("bytes_raw_total  : %" PRIu64 "\n", s->bytes_raw_total);
    printf("bytes_cmp_total  : %" PRIu64 "\n", s->bytes_cmp_total);
    printf("writes_total     : %" PRIu64 "\n", s->writes_total);
    printf("reads_total      : %" PRIu64 "\n", s->reads_total);

    printf("dict_id          : %" PRIu32 "\n", s->dict_id);
    printf("dict_size        : %" PRIu32 "\n", s->dict_size);
    printf("total_dicts      : %" PRIu32 "\n", s->total_dicts);
    printf("train_mode       : %" PRIu32 "\n", s->train_mode);
    printf("retrain_count    : %" PRIu32 "\n", s->retrain_count);
    printf("last_retrain_ms  : %" PRIu64 "\n", s->last_retrain_ms);

    printf("trainer_runs     : %" PRIu64 "\n", s->trainer_runs);
    printf("trainer_errs     : %" PRIu64 "\n", s->trainer_errs);
    printf("trainer_ms_last  : %" PRIu64 "\n", s->trainer_ms_last);

    printf("reservoir_bytes  : %" PRIu64 "\n", s->reservoir_bytes);
    printf("reservoir_items  : %" PRIu64 "\n", s->reservoir_items);

    printf("shadow_pct       : %" PRIu32 "%%\n", s->shadow_pct);
    printf("shadow_samples   : %" PRIu64 "\n", s->shadow_samples);
    printf("shadow_raw_total : %" PRIu64 "\n", s->shadow_raw_total);
    printf("shadow_saved_bytes: %" PRId64 "\n", s->shadow_saved_bytes);

    printf("promotions       : %" PRIu32 "\n", s->promotions);
    printf("rollbacks        : %" PRIu32 "\n", s->rollbacks);

    printf("triggers_rise    : %" PRIu32 "\n", s->triggers_rise);
    printf("triggers_drop    : %" PRIu32 "\n", s->triggers_drop);
    printf("cooldown_win_left: %" PRIu32 "\n", s->cooldown_win_left);

    printf("compress_errs    : %" PRIu64 "\n", s->compress_errs);
    printf("decompress_errs  : %" PRIu64 "\n", s->decompress_errs);
    printf("dict_miss_errs   : %" PRIu64 "\n", s->dict_miss_errs);

    printf("skipped_comp_min_size: %" PRIu64 "\n", s->skipped_comp_min_size);
    printf("skipped_comp_max_size: %" PRIu64 "\n", s->skipped_comp_max_size);
    printf("skipped_comp_incomp  : %" PRIu64 "\n", s->skipped_comp_incomp);

    printf("===============================\n");
}

void mcz_stats_snapshot_dump_json(const mcz_stats_snapshot_t *s, const char *ns)
{
    if (!s) {
        printf("null\n");
        return;
    }

    printf("{\n");

    printf("  \"namespace\": \"%s\",\n", ns ? ns : "global");

    printf("  \"ewma_m\": %.6f,\n", s->ewma_m);
    printf("  \"baseline\": %.6f,\n", s->baseline);
    printf("  \"cr_current\": %.6f,\n", s->cr_current);

    printf("  \"bytes_raw_total\": %" PRIu64 ",\n", s->bytes_raw_total);
    printf("  \"bytes_cmp_total\": %" PRIu64 ",\n", s->bytes_cmp_total);
    printf("  \"writes_total\": %" PRIu64 ",\n", s->writes_total);
    printf("  \"reads_total\": %" PRIu64 ",\n", s->reads_total);

    printf("  \"dict_id\": %" PRIu32 ",\n", s->dict_id);
    printf("  \"dict_size\": %" PRIu32 ",\n", s->dict_size);
    printf("  \"total_dicts\": %" PRIu32 ",\n", s->total_dicts);
    printf("  \"train_mode\": %" PRIu32 ",\n", s->train_mode);
    printf("  \"retrain_count\": %" PRIu32 ",\n", s->retrain_count);
    printf("  \"last_retrain_ms\": %" PRIu64 ",\n", s->last_retrain_ms);

    printf("  \"trainer_runs\": %" PRIu64 ",\n", s->trainer_runs);
    printf("  \"trainer_errs\": %" PRIu64 ",\n", s->trainer_errs);
    printf("  \"trainer_ms_last\": %" PRIu64 ",\n", s->trainer_ms_last);

    printf("  \"reservoir_bytes\": %" PRIu64 ",\n", s->reservoir_bytes);
    printf("  \"reservoir_items\": %" PRIu64 ",\n", s->reservoir_items);

    printf("  \"shadow_pct\": %" PRIu32 ",\n", s->shadow_pct);
    printf("  \"shadow_samples\": %" PRIu64 ",\n", s->shadow_samples);
    printf("  \"shadow_raw_total\": %" PRIu64 ",\n", s->shadow_raw_total);
    printf("  \"shadow_saved_bytes\": %" PRId64 ",\n", s->shadow_saved_bytes);

    printf("  \"promotions\": %" PRIu32 ",\n", s->promotions);
    printf("  \"rollbacks\": %" PRIu32 ",\n", s->rollbacks);

    printf("  \"triggers_rise\": %" PRIu32 ",\n", s->triggers_rise);
    printf("  \"triggers_drop\": %" PRIu32 ",\n", s->triggers_drop);
    printf("  \"cooldown_win_left\": %" PRIu32 ",\n", s->cooldown_win_left);

    printf("  \"compress_errs\": %" PRIu64 ",\n", s->compress_errs);
    printf("  \"decompress_errs\": %" PRIu64 ",\n", s->decompress_errs);
    printf("  \"dict_miss_errs\": %" PRIu64 ",\n", s->dict_miss_errs);

    printf("  \"skipped_comp_min_size\": %" PRIu64 ",\n", s->skipped_comp_min_size);
    printf("  \"skipped_comp_max_size\": %" PRIu64 ",\n", s->skipped_comp_max_size);
    printf("  \"skipped_comp_incomp\": %" PRIu64 "\n", s->skipped_comp_incomp);

    printf("}\n");
}
