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
 * mcdc_dict_pool.h
 *
 * Implementation of the global dictionary pool.
 * Manages reference counts and lifetime of shared ZSTD_CDict / ZSTD_DDict
 * objects across namespaces and published tables.
 *
 * Key duties:
 *   - Retain/release APIs for mcdc_dict_meta_t.
 *   - Reference count tracking.
 *   - Final free when no users remain.
 */
#ifndef MCDC_DICT_POOL_H
#define MCDC_DICT_POOL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>   /* for FILE* */


#include "zstd.h"  /* ZSTD_{C,}Dict */
#include "mcdc_dict.h" /* mcdc_dict_meta_t (for dict_path/signature) */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize/teardown once at process startup/shutdown. */
int  mcdc_dict_pool_init(void);
void mcdc_dict_pool_shutdown(void);

/* Retain compiled dicts for this meta (keyed by dict_path or signature).
   If compiled dicts are not present, the function uses the provided pointers.
   Returns 0 on success. */
int mcdc_dict_pool_retain_for_meta(mcdc_dict_meta_t *m, char **err_out);


/* Release one retain for this meta (per namespace). */
void mcdc_dict_pool_release_for_meta(const mcdc_dict_meta_t *m, int32_t *ref_left, char **err_out);

/* Return the current reference count for a dictionary meta.
 * If not found in the pool, return -1.
 */
int mcdc_dict_pool_refcount_for_meta(const mcdc_dict_meta_t *meta);

/* Dump the current dictionary pool state to the given FILE*.
 * Example usage: mcdc_dict_pool_dump(stdout);
 */
void mcdc_dict_pool_dump(FILE *out);

char *make_key_from_meta(const mcdc_dict_meta_t *m);

#ifdef __cplusplus
}
#endif
#endif /* MCDC_DICT_POOL_H */
