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
 * mcz_compression.h
 *
 * Core compression/decompression API for memcached-Zstd (MCZ).
 *
 * Responsibilities:
 *   - Define context structures and thread-local caches for Zstd.
 *   - Expose functions to compress and decompress payloads.
 *   - Provide hooks for dictionary usage (CDict/DDict) and training integration.
 *
 * Design:
 *   - All compression state is kept in `mcz_ctx_t` and `tls_cache_t`.
 *   - Thread-local cache avoids expensive Zstd context recreation.
 *   - Copy-on-write dictionary table publishing for safe hot updates.
 *   - All exported symbols prefixed with `mcz_` for clarity.
 */
#ifndef MCZ_COMPRESSION_H
#define MCZ_COMPRESSION_H

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
#include "mcz_config.h"
#include "mcz_dict.h"

/* ---------- portable atomic_uintptr_t --------------------------------- */
#if defined(__STDC_NO_ATOMICS__)
    /* compiler has no C11 atomics at all */
    typedef volatile uintptr_t  atomic_uintptr_t;
#   define atomic_load_explicit(p,o)          (*(p))
#   define atomic_store_explicit(p,v,o)       (*(p) = (v))
#   define atomic_exchange_explicit(p,v,o)    (__sync_lock_test_and_set((p),(v)))

#elif !defined(atomic_uintptr_t)
/* compiler supports C11 atomics, but typedef may be missing       */
#ifndef __CDT_PARSER__          /* <-- hide from Eclipse indexer */
    typedef _Atomic uintptr_t  atomic_uintptr_t;
#else                           /* CDT fallback: no _Atomic keyword */
typedef uintptr_t atomic_uintptr_t;
#endif
#endif

/* ---------- sample node ------------------------------------------------ */
typedef struct sample_node_s {
    struct sample_node_s *next;
    size_t len;
    void *buf;
} sample_node_t;
/* === GC state (MPSC retired table list) === */
typedef struct mcz_retired_node_s {
    struct mcz_retired_node_s *next;
    struct mcz_table_s        *tab;        /* retired table */
    time_t                     retired_at; /* enqueue time */
} mcz_retired_node_t;


/* ---------- global context -------------------------------------------- */
typedef struct mcz_ctx_s {
    _Atomic(sample_node_t *) samples_head; /* MPSC list head (push-only) */
    _Atomic(size_t) bytes_pending; /* atomically updated         */
    //TODO: not used
    pthread_t trainer_tid;
    mcz_cfg_t cfg;
    _Atomic(uintptr_t) dict_table;                /* Current dictionary routing table */
    _Atomic(mcz_retired_node_t*) gc_retired_head; /* MPSC stack head */
    _Atomic(bool)               gc_stop;          /* signal to stop GC thread */
    pthread_t                   gc_tid;           /* GC thread id */
    _Atomic(bool)               train_active;     /* is training active */

} mcz_ctx_t;

/* ---- Thread-local cache: add generation + slots ---- */
typedef struct tls_cache_s {
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
    void      *scratch;
    size_t     cap;
} tls_cache_t;

/* forward declarations – no memcached.h needed here */
struct _stritem;     /* real 'item' struct in items.h   */
typedef struct _stritem item;

typedef struct _mc_resp mc_resp;  /* defined in memcached.c */


/* Global init / destroy */
int mcz_init(mcz_cfg_t *cfg);
void mcz_destroy(void);

/* Fast-path API for Memcached */

ssize_t mcz_decompress(const void *src,
        size_t src_size, void *dst, size_t dst_sz, uint16_t dict_id);
ssize_t mcz_maybe_compress(const void *src, size_t src_sz, const void* key, size_t key_sz,
                    void **dst, uint16_t *dict_id_out);
ssize_t mcz_orig_size(const void *src, size_t comp_size);

/* Return values
 *   >0  : decompressed length
 *    0  : either ITEM_ZSTD flag not set  *or*  item is chunked
 *   <0  : negative errno / ZSTD error code
 */
ssize_t mcz_maybe_decompress(const item *it, mc_resp    *resp);

const mcz_ctx_t *mcz_ctx(void);
mcz_ctx_t       *mcz_ctx_mut(void);

int mcz_cfg_init(mcz_cfg_t *cfg);

/* Feed raw samples for future dictionary training */
void mcz_sample(const void *buf, size_t len);

/* =======================  NEW STATS SECTION  ========================= */
typedef struct {
    _Atomic(uint64_t) train_ok; /* dictionaries successfully built     */
    _Atomic(uint64_t) train_small; /* dict < 1 KiB                        */
    _Atomic(uint64_t) train_err; /* ZDICT_trainFromBuffer() errors      */
} mcz_stats_t;

void mcz_get_stats(mcz_stats_t *out);

int mcz_reload_dictionaries(void);

#endif /* MCZ_COMPRESSION_H */
