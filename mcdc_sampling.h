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
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque node type used internally; exposed only for sizeof/forward decls if needed */
typedef struct full_sample_node_s full_sample_node_t;

/* Status snapshot */
typedef struct {
    bool    configured;           /* has init succeeded */
    bool    running;              /* background thread running */
    size_t  bytes_written;        /* current file size (bytes) */
    size_t  bytes_collected;      /* current bytes collected (bytes) */
    char    current_path[1024];   /* current file path ("" if none) */
} mcdc_sampler_status_t;

/* Initialize module with parameters from higher-level config.
 * Takes ownership of nothing; copies strings internally.
 * Returns 0 on success, <0 (negative errno) on error.
 */
int mcdc_sampler_init(const char *spool_dir,
                     double sample_p,
                     int sample_window_sec,
                     size_t spool_max_bytes);

/* Start/stop background thread explicitly. */
int  mcdc_sampler_start(void);
int mcdc_sampler_stop(void);

/* Producer API: Apply Bernoulli(p), deep-copy key/value, and enqueue.
 * Returns 1 if accepted+queued, 0 if skipped/disabled, <0 on error.
 */
int mcdc_sampler_maybe_record(const void *key, size_t klen,
                             const void *val, size_t vlen);

/* Get a status snapshot. 'out' must be non-NULL. */
void mcdc_sampler_get_status(mcdc_sampler_status_t *out);

/* For tests/shutdown: drain and free queued items without writing to disk. */
void mcdc_sampler_drain_queue(void);

#ifdef __cplusplus
}
#endif
