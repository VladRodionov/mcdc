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
 * mcdc_dict.h
 *
 * Implementation of dictionary metadata and routing table subsystem.
 * Handles manifest parsing/writing, dictionary saving/loading, and
 * copy-on-write publishing of new router tables.
 *
 * Key duties:
 *   - Manage mcdc_dict_meta_t lifecycle.
 *   - Build and publish mcdc_table_t atomically.
 *   - Namespace routing and ID lookups.
 */
#pragma once
#ifndef MCDC_DICT_H
#define MCDC_DICT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <stddef.h>
#include "zstd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- One dictionary ---- */
typedef struct mcdc_dict_meta_s {
    uint16_t          id;
    char             *dict_path;      /* abs path to .dict */
    char             *mf_path;        /* abs path to .mf   */
    time_t            created;        /* when trained */
    time_t            retired;        /* 0 => active (not retired); else RFC3339 parsed time */
    int               level;          /* suggested compress level (info) */
    char            **prefixes;       /* array of namespace/prefix strings */
    size_t            nprefixes;
    char             *signature;      /* parsed, not verified in Phase-1 */
    size_t            dict_size;      /* bytes in dict file */
    const ZSTD_CDict *cdict;          /* shared, read-only */
    const ZSTD_DDict *ddict;          /* shared, read-only */
} mcdc_dict_meta_t;

/* ---- One namespace/prefix -> list of dicts (newest first) ---- */
typedef struct mcdc_ns_entry_s {
    char              *prefix;        /* e.g. "feed:" or "default" */
    mcdc_dict_meta_t  **dicts;         /* array[ndicts], dicts[0] is active */
    size_t             ndicts;
} mcdc_ns_entry_t;

/* ---- Published router table (copy-on-write) ---- */
typedef struct mcdc_table_s {
    mcdc_ns_entry_t   **spaces;        /* array[nspaces] */
    size_t             nspaces;

    mcdc_dict_meta_t   *metas;         /* contiguous ownership of dict metas */
    size_t             nmeta;

    /* O(1) id -> dict meta (newest wins). 65536 pointers ~ 512 KiB. */
    mcdc_dict_meta_t   *by_id[65536];

    time_t             built_at;
    uint32_t           gen;           /* generation/epoch */
} mcdc_table_t;


/* API */

mcdc_table_t *mcdc_scan_dict_dir(const char *dir,
                               size_t max_per_ns,
                               int64_t id_quarantine_s,
                               int comp_level,
                               char **err_out);

const mcdc_dict_meta_t *mcdc_pick_dict(const mcdc_table_t *tab, const char *key, size_t klen);
const mcdc_dict_meta_t *mcdc_lookup_by_id(const mcdc_table_t *tab, uint16_t id);

int mcdc_save_dictionary_and_manifest(const char *dir,
                                     const void *dict_data, size_t dict_size,
                                     const char * const *prefixes, size_t nprefixes,
                                     int level,
                                     const char *signature,
                                     time_t created,
                                     time_t retired,
                                     mcdc_dict_meta_t *out_meta,
                                     char **err_out);
mcdc_table_t *table_clone_plus(const mcdc_table_t *old,
                              const mcdc_dict_meta_t *new_meta_in,
                              const ZSTD_CDict *cdict,
                              const ZSTD_DDict *ddict,
                              size_t max_per_ns,
                              char **err_out);
int mcdc_mark_dict_retired(mcdc_dict_meta_t *meta, time_t now, char **err_out);
int mcdc_next_available_id(const mcdc_dict_meta_t *metas, size_t n,
                          int64_t quarantine_s, uint16_t *out_id, char **err_out);
bool mcdc_has_default_dict(const mcdc_table_t *tab);
bool mcdc_is_default_ns(const mcdc_table_t *tab, const char *key, size_t klen);


#ifdef __cplusplus
}
#endif
#endif
