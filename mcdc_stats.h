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

#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // throughput
    _Atomic uint64_t bytes_raw_total;
    _Atomic uint64_t bytes_cmp_total;
    _Atomic uint64_t writes_total;
    _Atomic uint64_t reads_total;


    // shadow (not implemented yet)
    _Atomic uint64_t shadow_samples;
    _Atomic uint64_t shadow_raw_total;
    _Atomic int64_t  shadow_saved_bytes; // bytes_saved_candidate - bytes_saved_primary
    _Atomic uint32_t promotions, rollbacks;
    // drift detector (counters only; scalars snap from detector)
    _Atomic uint32_t triggers_rise, triggers_drop;
    // training
    _Atomic uint32_t retrain_count;
    _Atomic uint64_t last_retrain_ms;
    _Atomic uint64_t trainer_runs, trainer_errs;
    _Atomic uint64_t trainer_ms_last;
    _Atomic uint64_t reservoir_bytes, reservoir_items;
    // errors
    _Atomic uint64_t compress_errs, decompress_errs, dict_miss_errs;
    _Atomic uint64_t skipped_comp_min_size, skipped_comp_max_size, skipped_comp_incomp;

} mcdc_stats_atomic_t;

typedef struct {
    // scalar snapshot (non-atomic floats from detector)
    double ewma_m /* current value */, baseline;
    double cr_current;
    // copy of atomics
    uint64_t bytes_raw_total, bytes_cmp_total, writes_total, reads_total;
    uint32_t dict_id, dict_size, total_dicts;
    uint32_t train_mode;
    uint32_t retrain_count;
    uint64_t last_retrain_ms;
    uint64_t trainer_runs, trainer_errs, trainer_ms_last;
    uint64_t reservoir_bytes, reservoir_items;

    uint32_t shadow_pct;
    uint64_t shadow_samples;
    uint64_t shadow_raw_total;
    int64_t  shadow_saved_bytes;
    uint32_t promotions, rollbacks;

    uint32_t triggers_rise, triggers_drop, cooldown_win_left;
    uint64_t compress_errs, decompress_errs, dict_miss_errs;
    uint64_t skipped_comp_min_size, skipped_comp_max_size, skipped_comp_incomp;

} mcdc_stats_snapshot_t;

/* Immutable entry ➜ points to shared stats block */
typedef struct mcdc_stats_ns_entry_s {
    const char *name;                // owned by table (heap)
    size_t      name_len;            // cached length
    mcdc_stats_atomic_t *stats;       // owned separately; reused across rebuilds
    struct mcdc_stats_ns_entry_s *next;     // hash chain
} mcdc_stats_ns_entry_t;

/* Immutable hash table + refcount for RCU-lite */
typedef struct mcdc_ns_table_s {
    _Atomic uint32_t refcnt;         // active readers on this table
    size_t nbuckets;
    mcdc_stats_ns_entry_t **buckets;        // array[nbuckets], chains are immutable
} mcdc_ns_table_t;

/* Registry: one global pointer to current table */
typedef struct {
    _Atomic(mcdc_ns_table_t *) cur;   // atomic pointer to immutable table
    mcdc_stats_atomic_t global;       // global stats (always present)
    _Atomic(mcdc_stats_atomic_t *) default_stats; /* ptr to "default" stats */
    _Atomic uint8_t               only_default;  /* 1 if only "default" exists */
} mcdc_stats_registry_t;

mcdc_stats_atomic_t *mcdc_stats_global(void);

mcdc_stats_atomic_t *mcdc_stats_default(void);

// helper functions
void mcdc_stats_add_io(mcdc_stats_atomic_t* s, uint64_t raw, uint64_t cmp);
void mcdc_stats_inc_err(mcdc_stats_atomic_t* s, const char* kind);

// snapshot fill
void mcdc_stats_snapshot_fill(mcdc_stats_atomic_t* s,
                             mcdc_stats_snapshot_t* out);

int mcdc_stats_registry_global_init(size_t nbuckets);

mcdc_stats_registry_t *mcdc_stats_registry_global(void);

void mcdc_stats_registry_global_destroy(void);

int mcdc_stats_rebuild_from_list(const char **names, size_t N, size_t nbuckets_new);
mcdc_stats_atomic_t *
mcdc_stats_lookup_by_key(const char *key, size_t klen);

mcdc_stats_atomic_t *
mcdc_stats_lookup_by_ns(const char *nsp, size_t nsp_sz);

void mcdc_stats_snapshot_dump(const mcdc_stats_snapshot_t *s, const char *ns);
void mcdc_stats_snapshot_dump_json(const mcdc_stats_snapshot_t *s, const char *ns);

int mcdc_stats_is_default(mcdc_stats_atomic_t * stats, bool *res);

#ifdef __cplusplus
}
#endif

