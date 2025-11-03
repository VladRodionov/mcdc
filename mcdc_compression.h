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

#ifndef MCDC_COMPRESSION_H
#define MCDC_COMPRESSION_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>     /* <-- add this */
#include <time.h>
#include <stdarg.h>
#include <sys/uio.h>
#include <zstd.h>
#include <stdatomic.h>
#include <pthread.h>
#include "mcdc_config.h"
#include "mcdc_dict.h"
#include "mcdc_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- sample node ------------------------------------------------ */
typedef struct sample_node_s {
    struct sample_node_s *next;
    size_t len;
    void *buf;
} sample_node_t;
/* === GC state (MPSC retired table list) === */
typedef struct mcdc_retired_node_s {
    struct mcdc_retired_node_s *next;
    struct mcdc_table_s        *tab;        /* retired table */
    time_t                     retired_at; /* enqueue time */
} mcdc_retired_node_t;


/* ---------- global context -------------------------------------------- */
typedef struct mcdc_ctx_s {
    _Atomic(sample_node_t *) samples_head;        /* MPSC list head (push-only) */
    _Atomic(size_t) bytes_pending;                /* atomically updated         */
    //TODO: not used
    pthread_t trainer_tid;
    mcdc_cfg_t *cfg;                                /* mcz configuration */
    _Atomic(uintptr_t) dict_table;                /* Current dictionary routing table */
    _Atomic(mcdc_retired_node_t*) gc_retired_head; /* MPSC stack head */
    _Atomic(bool)               gc_stop;          /* signal to stop GC thread */
    pthread_t                   gc_tid;           /* GC thread id */
    _Atomic(bool)               train_active;     /* is training active */

} mcdc_ctx_t;

/* ---- Thread-local cache ---- */
typedef struct tls_cache_s {
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
    void      *scratch;
    size_t     cap;
} tls_cache_t;

typedef struct mcdc_reload_status_s {
    int       rc;            /* 0 = success, <0 = error */
    uint32_t  namespaces;
    uint32_t  dicts_loaded;
    uint32_t  dicts_new;
    uint32_t  dicts_reused;
    uint32_t  dicts_retired;
    uint32_t  dicts_failed;
    char      err[128];
} mcdc_reload_status_t;

/* Global init / destroy */
int mcdc_init(void);
void mcdc_destroy(void);

/* Fast-path API for Memcached */

ssize_t mcdc_decompress(const void *src,
                       size_t src_size, void *dst, size_t dst_sz, uint16_t dict_id);
ssize_t mcdc_maybe_compress(const void *src, size_t src_sz, const void* key, size_t key_sz,
                           void **dst, uint16_t *dict_id_out);
ssize_t mcdc_orig_size(const void *src, size_t comp_size);

/* Return values
 *   >0  : decompressed length
 *    0  : either ITEM_ZSTD flag not set  *or*  item is chunked
 *   <0  : negative errno / ZSTD error code
 */
ssize_t mcdc_maybe_decompress(const char *value,
                             size_t value_sz, const char *key, size_t key_sz, void **dst, uint16_t did);
void mcdc_report_dict_miss_err(const char *key, size_t klen);

void mcdc_report_decomp_err(const char *key, size_t klen);

const mcdc_ctx_t *mcdc_ctx(void);
mcdc_ctx_t       *mcdc_ctx_mut(void);

int mcdc_set_max_value_limit(size_t limit);

/* Feed raw samples for future dictionary training and file spooling*/
void mcdc_sample(const void *key, size_t klen, const void *value, size_t vlen);

mcdc_reload_status_t *mcdc_reload_dictionaries(void);

const char *
mcdc_match_namespace(const char *key, size_t klen,
                    const char **spaces, size_t nspaces);

int mcdc_get_stats_snapshot(mcdc_stats_snapshot_t *snap, const char *ns, size_t ns_sz);

const char ** mcdc_list_namespaces(size_t *count);

bool mcdc_dict_exists(uint16_t id);

#ifdef __cplusplus
}
#endif
#endif /* MCDC_COMPRESSION_H */
