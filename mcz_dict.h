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
 * mcz_dict.h
 *
 * Implementation of dictionary metadata and routing table subsystem.
 * Handles manifest parsing/writing, dictionary saving/loading, and
 * copy-on-write publishing of new router tables.
 *
 * Key duties:
 *   - Manage mcz_dict_meta_t lifecycle.
 *   - Build and publish mcz_table_t atomically.
 *   - Namespace routing and ID lookups.
 */
#pragma once
#ifndef MCZ_DICT_H
#define MCZ_DICT_H

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
typedef struct mcz_dict_meta_s {
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
} mcz_dict_meta_t;

/* ---- One namespace/prefix -> list of dicts (newest first) ---- */
typedef struct mcz_ns_entry_s {
    char              *prefix;        /* e.g. "feed:" or "default" */
    mcz_dict_meta_t  **dicts;         /* array[ndicts], dicts[0] is active */
    size_t             ndicts;
} mcz_ns_entry_t;

/* ---- Published router table (copy-on-write) ---- */
typedef struct mcz_table_s {
    mcz_ns_entry_t   **spaces;        /* array[nspaces] */
    size_t             nspaces;

    mcz_dict_meta_t   *metas;         /* contiguous ownership of dict metas */
    size_t             nmeta;

    /* O(1) id -> dict meta (newest wins). 65536 pointers ~ 512 KiB. */
    mcz_dict_meta_t   *by_id[65536];

    time_t             built_at;
    uint32_t           gen;           /* generation/epoch */
} mcz_table_t;


/* API */

mcz_table_t *mcz_scan_dict_dir(const char *dir,
                               size_t max_per_ns,
                               int64_t id_quarantine_s,
                               int comp_level,
                               char **err_out);

const mcz_dict_meta_t *mcz_pick_dict(const mcz_table_t *tab, const char *key, size_t klen);
const mcz_dict_meta_t *mcz_lookup_by_id(const mcz_table_t *tab, uint16_t id);

int mcz_save_dictionary_and_manifest(const char *dir,
                                     const void *dict_data, size_t dict_size,
                                     const char * const *prefixes, size_t nprefixes,
                                     int level,
                                     const char *signature,
                                     time_t created,
                                     time_t retired,
                                     mcz_dict_meta_t *out_meta,
                                     char **err_out);
mcz_table_t *table_clone_plus(const mcz_table_t *old,
                              const mcz_dict_meta_t *new_meta_in,
                              const ZSTD_CDict *cdict,
                              const ZSTD_DDict *ddict,
                              size_t max_per_ns,
                              char **err_out);
int mcz_mark_dict_retired(mcz_dict_meta_t *meta, time_t now, char **err_out);
int mcz_next_available_id(const mcz_dict_meta_t *metas, size_t n,
                          int64_t quarantine_s, uint16_t *out_id, char **err_out);
bool mcz_has_default_dict(const mcz_table_t *tab);

#ifdef __cplusplus
}
#endif
#endif
