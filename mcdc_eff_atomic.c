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
 * mcdc_eff_atomic.c
 *
 * "eff" stands for **efficiency**.
 *
 * This module tracks compression efficiency using an Exponentially Weighted
 * Moving Average (EWMA) of the compression ratio (compressed_size /
 * original_size, where lower is better). The tracker is updated in the hot
 * path using C11 atomics (no locks) to minimize overhead.
 *
 * Responsibilities:
 *   - Maintain a global efficiency tracker (`g_eff`) as a singleton.
 *   - Update EWMA on every compression observation.
 *   - Track baseline efficiency at retrain points (monotonic non-increasing).
 *   - Decide when retraining is needed, based on degradation relative
 *     to the baseline, time interval, and bytes processed thresholds.
 *   - Expose APIs to mark retrain completion, query state, and configure
 *     retraining parameters.
 *
 * Why atomics:
 *   - Hot-path updates must be lightweight: a few relaxed atomic ops + CAS.
 *   - Readers and trainer thread can access shared state concurrently without
 *     mutexes.
 *
 * Naming convention:
 *   - All functions/types prefixed with `mcdc_eff_*` are part of this efficiency
 *     tracking subsystem.
 */
#include "mcdc_eff_atomic.h"
#include "mcdc_stats.h"
#include "mcdc_utils.h"

/* ------- helpers ------- */
static inline double clamp01(double x){ return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
static inline uint64_t dbl2u64(double d){ uint64_t u; memcpy(&u,&d,sizeof u); return u; }
static inline double   u642dbl(uint64_t u){ double d; memcpy(&d,&u,sizeof d); return d; }

/* ------- module-scoped config (read-only after configure) ------- */
static mcdc_train_cfg_t g_cfg;          /* not atomic; single-writer-before-readers */
static bool            g_cfg_set = false;

/* ------- global tracker ------- */
static mcdc_eff_tracker_atomic_t g_eff;

mcdc_eff_tracker_atomic_t *mcdc_eff_instance(void) { return &g_eff; }

/* Single-threaded configuration; after this, g_cfg is treated as immutable. */
void mcdc_eff_configure(const mcdc_train_cfg_t *cfg)
{
    g_cfg.enable_training = cfg->enable_training;
    g_cfg.retraining_interval_s = cfg->retraining_interval_s; /* seconds */
    g_cfg.min_training_size = cfg->min_training_size;     /* bytes since last train */
    g_cfg.ewma_alpha = clamp01(cfg->ewma_alpha);            /* 0..1 */
    g_cfg.retrain_drop = cfg->retrain_drop < 0? 0.0: cfg->retrain_drop;
    /* publish */
    g_cfg_set = true;

    /* Also cache alpha for hot path (no synchronization needed) */
    g_eff.alpha = g_cfg.ewma_alpha;
}

void mcdc_eff_get_config(mcdc_train_cfg_t *out_cfg)
{
    if (!out_cfg) return;
    *out_cfg = g_cfg; /* plain copy; config is immutable after configure */
}

void mcdc_eff_init(uint64_t now_s)
{
    /* Assume mcdc_eff_configure() already called by a single thread */

    atomic_store_explicit(&g_eff.ewma_bits, dbl2u64(0.0), memory_order_relaxed);
    atomic_store_explicit(&g_eff.baseline_bits, dbl2u64(0.0), memory_order_relaxed);
    atomic_store_explicit(&g_eff.ewma_initialized, false, memory_order_relaxed);

    atomic_store_explicit(&g_eff.last_train_ts_s, now_s, memory_order_relaxed);
    atomic_store_explicit(&g_eff.bytes_since_train, 0, memory_order_relaxed);
}

/* Hot path: relaxed increments + one CAS on a 64-bit word. */
void mcdc_eff_on_observation(size_t original_bytes,
                                   size_t compressed_bytes)
{
    if (original_bytes == 0) return;

    const double sample = (double)compressed_bytes / (double)original_bytes;

    atomic_fetch_add_explicit(&g_eff.bytes_since_train, original_bytes, memory_order_relaxed);

    bool inited = atomic_load_explicit(&g_eff.ewma_initialized, memory_order_acquire);
    if (!inited) {
        uint64_t expect0 = dbl2u64(0.0);
        uint64_t seed    = dbl2u64(sample);
        if (atomic_compare_exchange_strong_explicit(&g_eff.ewma_bits, &expect0, seed,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            atomic_store_explicit(&g_eff.baseline_bits, seed, memory_order_release);
            atomic_store_explicit(&g_eff.ewma_initialized, true, memory_order_release);
            return;
        }
        atomic_store_explicit(&g_eff.ewma_initialized, true, memory_order_release);
    }

    const double a = g_eff.alpha; /* immutable after configure */
    while (1) {
        uint64_t old_bits = atomic_load_explicit(&g_eff.ewma_bits, memory_order_acquire);
        double   old_val  = u642dbl(old_bits);
        double   new_val  = a * sample + (1.0 - a) * old_val;
        uint64_t new_bits = dbl2u64(new_val);
        if (new_bits == old_bits ||
            atomic_compare_exchange_weak_explicit(&g_eff.ewma_bits, &old_bits, new_bits,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }
}

bool mcdc_eff_should_retrain(uint64_t now_s)
{
    if (!g_cfg.enable_training) { return false; }

    const int64_t min_int = g_cfg.retraining_interval_s > 0 ? g_cfg.retraining_interval_s : 0;
    const size_t  min_sz  = g_cfg.min_training_size;
    const double  th      = g_cfg.retrain_drop > 0.0 ? g_cfg.retrain_drop : 0.0;

    bool     inited   = atomic_load_explicit(&g_eff.ewma_initialized, memory_order_acquire);
    uint64_t last_ts  = atomic_load_explicit(&g_eff.last_train_ts_s, memory_order_acquire);
    size_t   bytes    = atomic_load_explicit(&g_eff.bytes_since_train, memory_order_acquire);
    double   ewma     = u642dbl(atomic_load_explicit(&g_eff.ewma_bits, memory_order_acquire));
    double   base     = u642dbl(atomic_load_explicit(&g_eff.baseline_bits, memory_order_acquire));


    /* Need time+bytes gates always */
    if ((int64_t)(now_s - last_ts) < min_int) return false;
    if (bytes < min_sz) return false;

    /* --- Bootstrap: no baseline yet -> do an initial training pass --- */
    if (!inited || base <= 0.0) {
        return true;  /* trigger first training without drop check */
    }

    /* --- Steady state: require relative degradation --- */
    double rel = (ewma / base) - 1.0; /* >0 => worse compression ; < 0 - better compression, but still can be a different workload */
    bool trigger_up = rel <= -th;
    bool trigger_down = rel >= th;
    /* get statistics for "default" namespace*/
    mcdc_stats_atomic_t * stats = mcdc_stats_lookup_by_ns("default", 7);
    if (trigger_up){
        if (stats) atomic_inc32(&stats->triggers_rise, 1);
    } else if (trigger_down){
        if (stats) atomic_inc32(&stats->triggers_drop, 1);
    }
    return trigger_up || trigger_down;
}

/* Trainer thread only: keep baseline non-increasing.
   Assumes no concurrent writers to baseline_bits. */
static inline void baseline_fmin_trainer_only(double candidate) {
    double base = u642dbl(atomic_load_explicit(&g_eff.baseline_bits, memory_order_acquire));
    double newb = (base <= 0.0) ? candidate            /* first baseline */
                                : (candidate < base ? candidate : base);
    /* Single-writer: plain store is fine; use release for clarity. */
    atomic_store_explicit(&g_eff.baseline_bits, dbl2u64(newb), memory_order_release);
}

void mcdc_eff_mark_retrained(uint64_t now_s)
{
    /* Trainer thread calls this after publishing new dict */
    double cur = u642dbl(atomic_load_explicit(&g_eff.ewma_bits, memory_order_acquire));
    baseline_fmin_trainer_only(cur);  /* baseline := min(baseline, cur) or adopt on first */

    /* Reset counters / timestamp (visible to readers) */
    atomic_store_explicit(&g_eff.bytes_since_train, 0, memory_order_release);
    atomic_store_explicit(&g_eff.last_train_ts_s,   now_s, memory_order_release);
}

/* 1. Return current EWMA as double */
double mcdc_eff_get_ewma(void)
{
    uint64_t bits = atomic_load_explicit(&g_eff.ewma_bits, memory_order_acquire);
    return u642dbl(bits);
}

/* 2. Return baseline as double */
double mcdc_eff_get_baseline(void)
{
    uint64_t bits = atomic_load_explicit(&g_eff.baseline_bits, memory_order_acquire);
    return u642dbl(bits);
}

/* 3. Return seconds since last training */
uint64_t mcdc_eff_last_train_seconds(void)
{
    uint64_t last = atomic_load_explicit(&g_eff.last_train_ts_s, memory_order_acquire);
    return last;
}
