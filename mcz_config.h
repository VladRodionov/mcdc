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

typedef enum {
    MCZ_TRAIN_FAST = 0,
    MCZ_TRAIN_OPTIMIZE = 1,
} mcz_train_mode_t;

typedef struct {
    // Core
    bool     enable_comp;           // default true
    bool     enable_dict;           // default true
    char    *dict_dir;              // path
    size_t   dict_size;             // bytes (target dict size)
    int      zstd_level;            // reuse existing
    size_t   min_comp_size;         // compress if >=
    size_t   max_comp_size;         // compress if <=
    bool     compress_keys;         // compress key (false, not implemented yet)

    // Training
    bool     enable_training;       // enable online training
    int64_t  retraining_interval_s; // seconds
    size_t   min_training_size;     // bytes of eligible data since last train
    double   ewma_alpha;            // 0..1
    double   retrain_drop;          // 0..1
    mcz_train_mode_t train_mode;    // FAST (default) or OPTIMIZE
    
    // GC
    int32_t gc_cool_period;         // default, 1h - time to keep retired dictionary data in memory
    int32_t gc_quarantine_period;   // default: 7d, time to keep retired dictionary in a file system
    // Retention
    int      dict_retain_max;       // cap count of resident old dicts

    // Sampling + Spool
    bool     enable_sampling;       // enable sample spooling
    double   sample_p;              // 0..1
    int      sample_window_duration;// seconds
    char    *spool_dir;             // path
    size_t   spool_max_bytes;       // cap; drop-oldest windows
} mcz_cfg_t;


/* Default config values for MCZ */

#define MCZ_DEFAULT_ENABLE_COMP            true
#define MCZ_DEFAULT_ENABLE_DICT            true
#define MCZ_DEFAULT_DICT_DIR               NULL
#define MCZ_DEFAULT_DICT_SIZE              (256 * 1024)
#define MCZ_DEFAULT_ZSTD_LEVEL             3
#define MCZ_DEFAULT_MIN_COMP_SIZE          32
#define MCZ_DEFAULT_MAX_COMP_SIZE          (100 * 1024)

#define MCZ_DEFAULT_ENABLE_TRAINING        true
#define MCZ_DEFAULT_RETRAIN_INTERVAL_S     (2 * 60 * 60)
#define MCZ_DEFAULT_MIN_TRAINING_SIZE      0
#define MCZ_DEFAULT_EWMA_ALPHA             0.05
#define MCZ_DEFAULT_RETRAIN_DROP           0.1
#define MCZ_DEFAULT_TRAIN_MODE             MCZ_TRAIN_FAST

#define MCZ_DEFAULT_GC_COOL_PERIOD         3600
#define MCZ_DEFAULT_GC_QUARANTINE_PERIOD   (3600 * 24 * 7)

#define MCZ_DEFAULT_DICT_RETAIN_MAX        10

#define MCZ_DEFAULT_ENABLE_SAMPLING        true
#define MCZ_DEFAULT_SAMPLE_P               0.02
#define MCZ_DEFAULT_SAMPLE_WINDOW_DURATION 0
#define MCZ_DEFAULT_SPOOL_DIR              NULL
#define MCZ_DEFAULT_SPOOL_MAX_BYTES        (64 * 1024 * 1024)

#define MCZ_DEFAULT_COMPRESS_KEYS          false

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

void mcz_config_print(const mcz_cfg_t *cfg);

mcz_cfg_t * mcz_config_get(void);

void mcz_init_default_config(void);

int mcz_config_sanity_check(void);
#endif /* MCZ_CONFIG_H */
