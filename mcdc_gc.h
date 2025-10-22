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
 * mcdc_gc.h
 *
 * Implementation of garbage collector (GC) for retired dictionary tables.
 * Enqueues old mcdc_table_t instances and reclaims them after a cooling-off
 * period. Runs as a background thread draining the retired stack.
 *
 * Key duties:
 *   - Background GC loop (thread).
 *   - Retired table enqueue/drain.
 *   - Table + dictionary metadata cleanup.
 *   - Optional unlink of obsolete files.
 */
#ifndef MCDC_GC_H
#define MCDC_GC_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mcdc_ctx_s;    typedef struct mcdc_ctx_s    mcdc_ctx_t;
struct mcdc_table_s;  typedef struct mcdc_table_s  mcdc_table_t;
struct mcdc_retired_node_s; typedef struct mcdc_retired_node_s mcdc_retired_node_t;

/* Start GC thread;  */
int  mcdc_gc_start(mcdc_ctx_t *ctx);

/* Signal GC to stop and join the thread. Safe to call multiple times. */
void mcdc_gc_stop(mcdc_ctx_t *ctx);

/* Enqueue a retired table (called by publisher). Non-blocking MPSC push. */
void mcdc_gc_enqueue_retired(mcdc_ctx_t *ctx, mcdc_table_t *old_tab);

/* Free a routing table and all its allocations (no file I/O). */
void mcdc_free_table(mcdc_table_t *tab);

#ifdef __cplusplus
}
#endif

#endif /* MCDC_GC_H */
