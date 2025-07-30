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

/* ---------- eclipse_friendly_atomic.h ------------------------------ */
#ifdef __CDT_PARSER__
#define _Atomic(T)  T          /* strip for CDT indexer */
#endif
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
/* -------------------------------------------------------------------- */

/* User-tunable parameters */
typedef struct {
    int level; /* compression level (1-22), 0 = ZSTD default  */
    size_t max_dict; /* max dictionary size, bytes (≤ 220 KB)       */
    size_t min_train_bytes; /* train when collected bytes ≥ this threshold */
    const char *dict_dir_path; /* NULL ⇒ train live, else preload file  */
    /* Note: if dict_path is set, the trainer thread will not run. */
} zstd_cfg_t;

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
int zstd_init(zstd_ctx_t **out, const zstd_cfg_t *cfg);
void zstd_destroy(zstd_ctx_t *ctx);

/* Fast-path API for Memcached */
ssize_t zstd_compress_iov(zstd_ctx_t *ctx, const struct iovec *src, int src_cnt,
        void **dst, size_t *dst_cap, uint16_t *dict_id_out);

ssize_t zstd_decompress_into_iov(zstd_ctx_t *ctx, const void *src,
        size_t src_size, const struct iovec *dst, int dst_cnt, uint16_t dict_id);

/* Feed raw samples for future dictionary training */
void zstd_sample(zstd_ctx_t *ctx, const void *buf, size_t len);

/* ----- public query helpers ---------------------------------------- */
bool zstd_dict_ready(const zstd_ctx_t *ctx); /* has final dict   */

/* =======================  NEW STATS SECTION  ========================= */
typedef struct {
    _Atomic(uint64_t) train_ok; /* dictionaries successfully built     */
    _Atomic(uint64_t) train_small; /* dict < 1 KiB                        */
    _Atomic(uint64_t) train_err; /* ZDICT_trainFromBuffer() errors      */
} zstd_stats_t;

void zstd_get_stats(zstd_stats_t *out);

#endif /* ZSTD_COMPRESSION_H */
