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
 * mcz_gc.h
 *
 * Implementation of garbage collector (GC) for retired dictionary tables.
 * Enqueues old mcz_table_t instances and reclaims them after a cooling-off
 * period. Runs as a background thread draining the retired stack.
 *
 * Key duties:
 *   - Background GC loop (thread).
 *   - Retired table enqueue/drain.
 *   - Table + dictionary metadata cleanup.
 *   - Optional unlink of obsolete files.
 */
#ifndef MCZ_GC_H
#define MCZ_GC_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mcz_ctx_s;    typedef struct mcz_ctx_s    mcz_ctx_t;
struct mcz_table_s;  typedef struct mcz_table_s  mcz_table_t;
struct mcz_retired_node_s; typedef struct mcz_retired_node_s mcz_retired_node_t;

/* Start GC thread;  */
int  mcz_gc_start(mcz_ctx_t *ctx);

/* Signal GC to stop and join the thread. Safe to call multiple times. */
void mcz_gc_stop(mcz_ctx_t *ctx);

/* Enqueue a retired table (called by publisher). Non-blocking MPSC push. */
void mcz_gc_enqueue_retired(mcz_ctx_t *ctx, mcz_table_t *old_tab);

/* Free a routing table and all its allocations (no file I/O). */
void mcz_free_table(mcz_table_t *tab);

#ifdef __cplusplus
}
#endif

#endif /* MCZ_GC_H */
