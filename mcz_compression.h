#ifndef ZSTD_COMPRESSION_H
#define ZSTD_COMPRESSION_H

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

/* ---------- global context -------------------------------------------- */
typedef struct zstd_ctx_s {
    _Atomic(sample_node_t *) samples_head; /* MPSC list head (push-only) */
    _Atomic(size_t) bytes_pending; /* atomically updated         */
    _Atomic(bool) dict_ready; /* set exactly once by trainer   */
    _Atomic(uint16_t) cur_dict_id;  /* 0 = none, 1,2,… = active ID */

    atomic_uintptr_t cdict; /* current compression dictionary */
    atomic_uintptr_t ddict; /* current decompression dictionary */
    pthread_t trainer_tid;
    zstd_cfg_t cfg;
} zstd_ctx_t;

/* Opaque context handle */
/* typedef struct zstd_ctx_s zstd_ctx_t;*/

/* Global init / destroy */
int zstd_init(zstd_cfg_t *cfg);
void zstd_destroy(void);

/* Fast-path API for Memcached */

ssize_t zstd_decompress(const void *src,
        size_t src_size, void *dst, size_t dst_sz, uint16_t dict_id);
ssize_t zstd_maybe_compress(const void *src, size_t src_sz,
                    void **dst, uint16_t *dict_id_out);
ssize_t zstd_orig_size(const void *src, size_t comp_size);
/* Feed raw samples for future dictionary training */
void zstd_sample(const void *buf, size_t len);

/* =======================  NEW STATS SECTION  ========================= */
typedef struct {
    _Atomic(uint64_t) train_ok; /* dictionaries successfully built     */
    _Atomic(uint64_t) train_small; /* dict < 1 KiB                        */
    _Atomic(uint64_t) train_err; /* ZDICT_trainFromBuffer() errors      */
} zstd_stats_t;

void zstd_get_stats(zstd_stats_t *out);


/* forward declarations – no memcached.h needed here */
struct _stritem;     /* real 'item' struct in items.h   */
typedef struct _stritem item;

typedef struct _mc_resp mc_resp;  /* defined in memcached.c */

/* Return values
 *   >0  : decompressed length
 *    0  : either ITEM_ZSTD flag not set  *or*  item is chunked
 *   <0  : negative errno / ZSTD error code
 */
ssize_t zstd_maybe_decompress(const item *it, mc_resp    *resp);

const zstd_ctx_t *zstd_ctx(void);
zstd_ctx_t       *zstd_ctx_mut(void);

int zstd_cfg_init(zstd_cfg_t *cfg);
#endif /* ZSTD_COMPRESSION_H */
