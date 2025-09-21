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
 * mcz_config.h
 *
 * Implementation of configuration management for MCZ.
 *
 * Key duties:
 *   - Parse and validate configuration sources.
 *   - Initialize config structures with defaults.
 *   - Expose safe accessors to other modules.
 */
#ifndef MCZ_CONFIG
#define MCZ_CONFIG

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------------
 * User-tunable parameters for the zstd integration
 * (keep in sync with mcz_compression.h)
 * ------------------------------------------------------------------ */
typedef struct {
    // Core
    bool     enable_comp;           // default true
    bool     enable_dict;           // default true
    char    *dict_dir;              // path
    size_t   dict_size;             // bytes (target dict size)
    int      zstd_level;            // reuse existing
    size_t   min_comp_size;         // compress if >=
    size_t   max_comp_size;         // compress if <=
    double   min_savings;           // 0..1 (skip if below)
    bool     compress_keys;         // compress key (false, not implemented yet)

    // Training
    bool     enable_training;       // enable online training
    int64_t  retraining_interval_s; // seconds
    size_t   min_training_size;     // bytes of eligible data since last train
    double   ewma_alpha;            // 0..1
    double   retrain_drop;          // 0..1
    // GC
    int32_t gc_run_interval;
    int32_t gc_cool_period;         // default, 1h - time to keep retired dictionary data in memory
    int32_t gc_quarantine_period;    // default: 7d, time to keep retired dictionary in a file system
    // Retention
    int      dict_retain_hours;     // keep dicts <= N hours
    int      dict_retain_max;       // cap count of resident old dicts

    // Sampling + Spool
    bool     enable_sampling;       // enable sample spooling
    double   sample_p;              // 0..1
    int      sample_window_sec;     // seconds
    size_t   sample_roll_bytes;     // rotate when >=
    char    *spool_dir;             // path
    size_t   spool_max_bytes;       // cap; drop-oldest windows
    //enum { MCZ_KEYMODE_RAW=0, MCZ_KEYMODE_PREFIX=1, MCZ_KEYMODE_HASH=2 } sample_key_mode;
    //int      sample_prefix_n;       // valid if KEYMODE_PREFIX
} mcz_cfg_t;

/* --------------------------------------------------------------------
 * parse_mcz_config()
 *
 *  - Reads an INI-style file (key = value, “#” comments).
 *  - Recognised keys: level, max_dict, min_train_size,
 *    min_comp_size, max_comp_size, compress_keys, dict_dir_path.
 *  - Size values accept K/M/G suffixes (case-insensitive).
 *  - Populates / overrides fields in *cfg.
 *
 * Returns:
 *      0          on success
 *     -errno      on I/O failure (-ENOENT, -EACCES, …)
 *     -EINVAL     on syntax error
 *-------------------------------------------------------------------*/
int parse_mcz_config(const char *path);

#endif /* MCZ_CONFIG_H */
