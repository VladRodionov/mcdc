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
 * mcz_eff_atomic.h
 *
 * Efficiency tracker ("eff") for compression dictionaries.
 *
 * Tracks an Exponentially Weighted Moving Average (EWMA) of compression
 * efficiency (compressed_size / original_size). Provides lock-free APIs
 * for:
 *   - Recording compression observations (hot path).
 *   - Querying current EWMA and baseline efficiency.
 *   - Determining when retraining should occur.
 *   - Marking retrain completion and updating baseline.
 *   - Configuring retrain thresholds (interval, bytes, drop, etc.).
 *
 * Notes:
 *   - Baseline is non-increasing: it only improves with retraining.
 *   - Atomic-only design: no mutexes in the hot path.
 *   - Global singleton instance (like mcz_ctx): no need to pass a tracker
 *     around.
 */
#ifndef MCZ_EFF_ATOMIC_H
#define MCZ_EFF_ATOMIC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   /* memcpy */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Config (owned by module; read-only after mcz_eff_configure) --- */
typedef struct mcz_train_cfg_s {
    bool     enable_training;       /* enable online training */
    int64_t  retraining_interval_s; /* seconds */
    size_t   min_training_size;     /* bytes since last train */
    double   ewma_alpha;            /* 0..1 */
    double   retrain_drop;          /* 0..1 (relative increase threshold for ratio) */
} mcz_train_cfg_t;

/* --- Lock-free EWMA tracker (singleton) ---
 Tracks compression ratio = comp/orig (lower is better). */
typedef struct mcz_eff_tracker_atomic_s {
    _Atomic uint64_t ewma_bits;       /* bit-cast double EWMA */
    _Atomic uint64_t baseline_bits;   /* bit-cast double at last retrain */
    _Atomic bool     ewma_initialized;

    _Atomic uint64_t last_train_ts_s; /* epoch seconds at last retrain */
    _Atomic size_t   bytes_since_train;

    double alpha;                     /* cached from config at configure/init */
} mcz_eff_tracker_atomic_t;

/* Access the singleton tracker (optional/testing). */
mcz_eff_tracker_atomic_t *mcz_eff_instance(void);

/* --- Configuration (call once, single thread; afterwards read-only) --- */
void mcz_eff_configure(const mcz_train_cfg_t *cfg);
/* Optional: copy out the current config */
void mcz_eff_get_config(mcz_train_cfg_t *out_cfg);

/* --- Lifecycle & operations  --- */
void mcz_eff_init(uint64_t now_s);

void mcz_eff_on_observation(size_t original_bytes,
                            size_t compressed_bytes);

bool mcz_eff_should_retrain(uint64_t now_s);

void mcz_eff_mark_retrained(uint64_t now_s);

double mcz_eff_get_ewma(void);

double mcz_eff_get_baseline(void);

uint64_t mcz_eff_last_train_seconds(void);

#ifdef __cplusplus
extern "C" {
#endif

#endif /* MCZ_EFF_ATOMIC_H */

