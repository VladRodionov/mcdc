#include "zstd_compression.h"

#include <pthread.h>
#include <stdio.h>          /* FILE, fopen, fread, fclose */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>         /* usleep                     */
#include <zdict.h>          /* ZDICT_trainFromBuffer      */
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>         /* PATH_MAX                   */
/*  add these two lines  */
#include <sys/types.h>
#include <sys/stat.h>
#include "memcached.h"  /* memcached.h                */
/* ---------- TLS cache per worker thread ----------------------------- */
typedef struct {
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
    void *scratch;
    size_t cap;
} tls_cache_t;

static __thread tls_cache_t tls; /* zero-initialised */

/* ---------- zstd context --------------------------------------------- */
static zstd_ctx_t g_zstd;      /* zero-init by the loader */

/* ---------- zstd context helpers ------------------------------------ */
const zstd_ctx_t *zstd_ctx(void)      { return &g_zstd; }
zstd_ctx_t       *zstd_ctx_mut(void)  { return &g_zstd; }

/* ---------- helper macros ------------------------------------------- */
#define CDICT(ctx) ((ZSTD_CDict*)atomic_load_explicit(&(ctx)->cdict, memory_order_acquire))
#define DDICT(ctx) ((ZSTD_DDict*)atomic_load_explicit(&(ctx)->ddict, memory_order_acquire))

static void tls_ensure(size_t need) {
    if (!tls.cctx)
        tls.cctx = ZSTD_createCCtx();
    if (!tls.dctx)
        tls.dctx = ZSTD_createDCtx();
    if (need > tls.cap) {
        tls.scratch = realloc(tls.scratch, need);
        tls.cap = need;
    }
}

static zstd_stats_t zstd_stats = { 0 };

/* Optional helper you can expose via “stats zstd” command */
void zstd_get_stats(zstd_stats_t *out) {
    *out = zstd_stats;
}

/* ----------------------------------------------------------------------
 * log_rate_limited()
 *   Prints to stderr at most once every `interval_us` micro-seconds.
 *   Uses a static timestamp; thread-safe under POSIX (atomic exchange).
 * -------------------------------------------------------------------- */

static uint64_t now_usec(void) /* monotonic wall-clock */
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

static void log_rate_limited(uint64_t interval_us, const char *fmt, ...) {

    /* time of last log */
    static _Atomic(uint64_t) last_ts = 0;

    uint64_t now = now_usec();
    uint64_t prev = atomic_load_explicit(&last_ts, memory_order_relaxed);

    if (now - prev < interval_us)
        return; /* still within quiet window */

    /* attempt to claim the slot */
    if (!atomic_compare_exchange_strong_explicit(&last_ts, &prev, now,
            memory_order_acq_rel, memory_order_relaxed))
        return; /* another thread logged */

    /* we won the race → emit message */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int str_to_u16(const char *s, uint16_t *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v <= 0 || v > 0xFFFF)
        return -EINVAL;
    *out = (uint16_t) v;
    return 0;
}

/* ---------------------------------------------------------------------
 * loadDictionary()
 *   If ctx->cfg.dict_path is non-NULL, reads the file, builds CDict/DDict,
 *   marks ctx->dict_ready = true, and returns 0.
 *   If dict_path == NULL it returns 1 (nothing loaded, continue live training).
 *   On I/O/alloc/ZSTD errors returns a negative errno.
 * ------------------------------------------------------------------- */
static int zstd_load_dicts(void) {
    zstd_ctx_t *ctx = zstd_ctx_mut();

    if (!ctx->cfg.dict_dir_path)
        return 1; /* nothing to load, continue live training */

    DIR *d = opendir(ctx->cfg.dict_dir_path);
    if (!d)
        return -ENOENT;

    struct dirent *de;
    uint16_t id = 0;
    char full[PATH_MAX] = { 0 };

    while ((de = readdir(d))) {
        if (de->d_type == DT_DIR)
            continue;
        if (str_to_u16(de->d_name, &id) == 0) {
            snprintf(full, sizeof(full), "%s/%s", ctx->cfg.dict_dir_path,
                    de->d_name);
            break;
        }
    }
    closedir(d);
    if (!id)
        return -ENOENT; /* none found */

    /* read file */
    FILE *f = fopen(full, "rb");
    if (!f)
        return -errno; /* I/O error */

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        return 1; /* zero-length file: ignore silently */
    }

    void *buf = malloc(file_size);
    if (!buf) {
        fclose(f);
        return -ENOMEM;
    }

    size_t off = 0;
    while (off < (size_t) file_size) {
        size_t n = fread((char*) buf + off, 1, (size_t) file_size - off, f);
        if (n == 0) { /* I/O error or EOF too early */
            free(buf);
            fclose(f);
            free(buf);
            return -EIO;
        }
        off += n;
    }
    fclose(f);

    ZSTD_CDict *cd = ZSTD_createCDict(buf, file_size, ctx->cfg.level ? ctx->cfg.level : 3);
    ZSTD_DDict *dd = ZSTD_createDDict(buf, file_size);

    if (!cd || !dd) {
        free(buf);
        if (cd)
            ZSTD_freeCDict(cd);
        if (dd)
            ZSTD_freeDDict(dd);
        free(buf);
        return -ENOMEM; /* alloc failed */
    }
    free(buf);

    atomic_store_explicit(&ctx->cdict, (uintptr_t) cd, memory_order_release);
    atomic_store_explicit(&ctx->ddict, (uintptr_t) dd, memory_order_release);
    atomic_store_explicit(&ctx->cur_dict_id, id, memory_order_release);
    /* load file exactly as before ... */
    atomic_store(&ctx->dict_ready, true); /* disable training */
    return 0;
}

static int zstd_save_dict(const void *dict, size_t dict_size,
        uint16_t dict_id, bool overwrite) {

    zstd_ctx_t *ctx = zstd_ctx_mut();

    const char *dir_path = ctx->cfg.dict_dir_path;
    if (!dict || !dict_size || !dir_path || dict_id == 0)
        return -EINVAL;
    /* -- ensure directory exists ----------------------------------- */
    struct stat st;
    if (stat(dir_path, &st) == -1) {
        if (errno == ENOENT) {
            /* try to create it */
            if (mkdir(dir_path, 0755) == -1 && errno != EEXIST)
                return -errno;
        } else {
            return -errno;                /* stat failed for other reason */
        }
    } else if (!S_ISDIR(st.st_mode)) {
        return -ENOTDIR;                  /* path exists but is not a dir */
    }
    /* build path "<dir>/<id>" */
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%hu", dir_path, dict_id);
    if (n <= 0 || n >= (int)sizeof(path))
        return -ENAMETOOLONG;

    int flags = O_WRONLY | O_CREAT | (overwrite ? O_TRUNC : O_EXCL);
    int fd = open(path, flags, 0644);
    if (fd == -1)
        return -errno;

    const char *p = dict;
    size_t nleft = dict_size;
    while (nleft) {
        ssize_t w = write(fd, p, nleft);
        if (w <= 0) {          /* error or wrote zero bytes */
            int e = errno;
            close(fd);
            return e ? -e : -EIO;
        }
        p      += w;
        nleft  -= (size_t)w;
    }
    close(fd);
    return 0;
}

/* ---------- trainer thread ------------------------------------------ */
static void* trainer_main(void *arg) {
    zstd_ctx_t *ctx = arg;
    size_t max_dict = ctx->cfg.max_dict ? ctx->cfg.max_dict : 110 * 1024;
    size_t train_threshold =
            ctx->cfg.min_train_size ?
                    ctx->cfg.min_train_size : max_dict * 100; /* 100× rule */

    while (1) {
        usleep(100000); /* 100 ms */
        bool success = false;
        if (atomic_load_explicit(&ctx->bytes_pending, memory_order_acquire)
                < train_threshold)
            continue;
        /* Atomically take ownership of the whole list */
        sample_node_t *list = atomic_exchange_explicit(&ctx->samples_head,
        NULL, memory_order_acq_rel);
        if (!list)
            continue; /* rare race */
        /* Walk list, count nodes & bytes, fill arrays */
        size_t count = 0, total = 0;
        for (sample_node_t *n = list; n; n = n->next) {
            count++;
            total += n->len;
        }
        size_t *sizes = malloc(sizeof(size_t) * count);
        void **buffs = malloc(sizeof(void*) * count);
        sample_node_t *n = list;
        for (size_t i = 0; i < count; ++i, n = n->next) {
            sizes[i] = n->len;
            buffs[i] = n->buf;
        }
        /* train dictionary */
        void *dict = malloc(max_dict);
        size_t dict_sz = ZDICT_trainFromBuffer(dict, max_dict, buffs, sizes,
                count);

        if (ZSTD_isError(dict_sz)) { /* hard failure      */
            log_rate_limited(10 * 1000000ULL, /* 10 s */
            "zstd-dict: TRAIN ERROR %s (samples=%zu, bytes=%zu)\n",
                    ZSTD_getErrorName(dict_sz), count, total);
            atomic_fetch_add_explicit(&zstd_stats.train_err, 1,
                    memory_order_relaxed);
        } else if (dict_sz < 1024) { /* too small         */
            log_rate_limited(10 * 1000000ULL,
                    "zstd-dict: dict too small (%zu B, need ≥1 KiB)\n",
                    dict_sz);
            atomic_fetch_add_explicit(&zstd_stats.train_small, 1,
                    memory_order_relaxed);
        } else {
            ZSTD_CDict *nc = ZSTD_createCDict(dict, dict_sz,
                    ctx->cfg.level ? ctx->cfg.level : 3);
            ZSTD_DDict *nd = ZSTD_createDDict(dict, dict_sz);

            if (nc && nd) {

                /* atomically swap in new dicts */
                /* Note: we use acquire-release semantics to ensure that
                 * the new CDict/DDict is visible to all threads after this point.
                 * This code works only for one shot dictionary training, 0 -> 1
                 * transition. If you want to support multiple training sessions,
                 * we need different storage for trained dictionaries - map (id - CDict, id - DDict
                 */
                uintptr_t oldc = atomic_exchange_explicit(&ctx->cdict,
                        (uintptr_t) nc, memory_order_acq_rel);
                uintptr_t oldd = atomic_exchange_explicit(&ctx->ddict,
                        (uintptr_t) nd, memory_order_acq_rel);
                if (oldc)
                    ZSTD_freeCDict((ZSTD_CDict*) oldc);
                if (oldd)
                    ZSTD_freeDDict((ZSTD_DDict*) oldd);
                /* keep `dict` alive: owned by nc/nd */
                /* SUCCESS → publish */
                /* Increment global dictionary ID atomically                            */
                uint16_t new_id = atomic_load_explicit(&ctx->cur_dict_id,
                                                       memory_order_relaxed) + 1;
                if (new_id == 0) new_id = 1;        /* wrap 0xFFFF → 1 */
                atomic_store_explicit(&ctx->cur_dict_id, new_id, memory_order_release);
                atomic_store_explicit(&ctx->dict_ready, true, memory_order_release);
                log_rate_limited(1000000ULL, /* 1 s */
                "zstd-dict: new dict %u (%zu B) built from %zu samples\n",
                        new_id, dict_sz, count);
                atomic_fetch_add_explicit(&zstd_stats.train_ok, 1,
                        memory_order_relaxed);
                /* OPTIONAL: save the raw dictionary bytes for future cold-start */
                if (ctx->cfg.dict_dir_path) { /* reuse same path or another config field */
                    int rc = zstd_save_dict(dict, dict_sz, new_id, true);
                    if (rc)
                        log_rate_limited(10 * 1000000ULL,
                                "zstd-dict: could not save dict (%s): %s\n",
                                ctx->cfg.dict_dir_path, strerror(-rc));
                }
                success = true;
            } else {
                log_rate_limited(10 * 1000000ULL,
                        "zstd-dict: CDict/DDict alloc failed\n");
                atomic_fetch_add_explicit(&zstd_stats.train_err, 1,
                        memory_order_relaxed);
                if (nc)
                    ZSTD_freeCDict(nc);
                if (nd)
                    ZSTD_freeDDict(nd);
            }
        } // What to do if training fails?

        free(dict);
        /* Free nodes & sample buffers */
        n = list;
        while (n) {
            sample_node_t *tmp = n->next;
            free(n->buf);
            free(n);
            n = tmp;
        }
        free(buffs);
        free(sizes);
        if (success) {
            break;
        } else {
            atomic_fetch_sub_explicit(&ctx->bytes_pending, total,
                    memory_order_acq_rel);
        }
    }
    return NULL; /* never reached */
}

static inline bool dict_is_ready(void) {
    const zstd_ctx_t *c = zstd_ctx();
    /* any cheap acquire fence is fine; on x86 it's a compiler barrier */
    /* Do we really need this? - this will trash performance*/
    atomic_thread_fence(memory_order_acquire);
    return c->dict_ready;
}

/* ---------- public init / destroy ----------------------------------- */
int zstd_init( const zstd_cfg_t *cfg) {
    zstd_ctx_t *ctx = zstd_ctx_mut();
    if (!ctx)
        return -ENOMEM;
    /* 1. configuration */
    ctx->cfg = cfg ? *cfg : (zstd_cfg_t ) { .level = 3, .max_dict = 110 * 1024,
                             .min_train_size = 110 * 1024 * 100,
                             .min_comp_size = 32,
                             .max_comp_size = 64 * 1024,
                             .compress_keys = false,
                             .dict_dir_path = NULL };
    /* 2. atomic pointers / counters */
    atomic_init(&ctx->samples_head, NULL); /* empty list            */
    atomic_init(&ctx->bytes_pending, 0);
    atomic_init(&ctx->cdict, (uintptr_t) NULL);
    atomic_init(&ctx->ddict, (uintptr_t) NULL);
    atomic_init(&ctx->dict_ready, false);
    atomic_init(&ctx->cur_dict_id, 0); /* 0 = no dict, 1 = active */
    ctx->trainer_tid = (pthread_t ) { 0 }; /* will be set by trainer thread */

    /* try external dictionary first */
    int rc = zstd_load_dicts();
    if (rc < 0) {
        free(ctx);
        return rc;
    }
    if (rc == 0) {
        ctx->trainer_tid = pthread_self(); /* dummy thread ID *//* dictionary loaded → done     */
        return 0;
    }

    /* ---------------- spawn background trainer --------------------------- */

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&ctx->trainer_tid, &attr, trainer_main, ctx);
    pthread_attr_destroy(&attr);
    log_rate_limited(1000000ULL, /* 1 s */
    "zstd-dict: trainer thread started (max_dict=%zu B)\n", ctx->cfg.max_dict);

    return 0;
}

void zstd_destroy(void) {
    zstd_ctx_t *ctx = zstd_ctx_mut();

    /* note: trainer thread is detached and loops forever; in production
     you may add a stop flag + join, or just let process exit */
    ZSTD_CDict *cd = CDICT(ctx);
    ZSTD_DDict *dd = DDICT(ctx);
    if (cd)
        ZSTD_freeCDict(cd);
    if (dd)
        ZSTD_freeDDict(dd);

    free(ctx);

    /* free thread-local caches for the calling thread */
    if (tls.scratch) {
        free(tls.scratch);
        tls.scratch = NULL;
        tls.cap = 0;
    }
    if (tls.cctx) {
        ZSTD_freeCCtx(tls.cctx);
        tls.cctx = NULL;
    }
    if (tls.dctx) {
        ZSTD_freeDCtx(tls.dctx);
        tls.dctx = NULL;
    }
}

void zstd_sample(const void *src, size_t len) {
    zstd_ctx_t *ctx = zstd_ctx_mut();
    /* skip very large and very small items */
    if (len >ctx->cfg.max_comp_size || len < ctx->cfg.min_comp_size)
        return;
    if (dict_is_ready()) /* skip if dictionary is ready */
        return;
    /* ---- back-pressure: stop once corpus ≥ min_train_bytes ---------- */
    size_t limit =
            ctx->cfg.min_train_size ?
                    ctx->cfg.min_train_size : ctx->cfg.max_dict * 100; /* 100× rule */

    if (atomic_load_explicit(&ctx->bytes_pending, memory_order_relaxed)
            >= limit)
        return; /* trainer hasn’t processed yet     */

    /* ---- allocate sample node -------------------------------------- */
    sample_node_t *node = malloc(sizeof(*node));
    if (!node)
        return;
    node->buf = malloc(len);
    if (!node->buf) {
        free(node);
        return;
    }
    memcpy(node->buf, src, len);
    node->len = len;

    /* ---- lock-free push into MPSC list ------------------------------ */
    sample_node_t *old_head;
    do {
        old_head = atomic_load_explicit(&ctx->samples_head,
                memory_order_acquire);
        node->next = old_head;
    } while (!atomic_compare_exchange_weak_explicit(&ctx->samples_head,
            &old_head, node, memory_order_release, memory_order_relaxed));

    atomic_fetch_add_explicit(&ctx->bytes_pending, len, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Decompress a value into an iovec array.                             *
 *  - src/src_sz : compressed buffer                                   *
 *  - dst/dst_cnt: scatter-gather destination                          *
 *  - dict_id    : 0 = no dict, ≥1 = dictionary selector               *
 * Returns:                                                            *
 *    ≥0  decompressed bytes                                           *
 *   < 0  negative errno / ZSTD error code                             */
/* ------------------------------------------------------------------ */
static inline const ZSTD_DDict*
get_ddict_by_id(uint16_t id)
/* For now we support only one live dictionary (id==1). When you implement
 * rotation, change this helper to do a table lookup or array index.          */
{
    zstd_ctx_t *ctx = zstd_ctx_mut();
    return (id == 1) ? DDICT(ctx) : NULL;
}

static inline const ZSTD_CDict*
get_cdict_by_id(uint16_t id) {
    zstd_ctx_t *ctx = zstd_ctx_mut();
    /* For now we support only one live dictionary (id==1). When you implement
     * rotation, change this helper to do a table lookup or array index.          */
    return (id == 1) ? CDICT(ctx) : NULL;
}

/* -----------------------------------------------------------------
 * Compress an value.
 *  • On success: returns compressed size (≥0) and sets *dict_id_out.
 *  • On error  : returns negative errno / ZSTD error; *dict_id_out == 0.
 * ----------------------------------------------------------------*/
ssize_t zstd_maybe_compress(const void *src, size_t src_sz,
                    void **dst, uint16_t *dict_id_out)
{
    zstd_ctx_t *ctx = zstd_ctx_mut();          /* global-static instance */

    /* 0.  sanity checks ------------------------------------------ */
    if (!ctx || !src || src_sz == 0 || !dst || !dict_id_out)
        return -EINVAL;

    if ((ctx->cfg.min_comp_size && src_sz < ctx->cfg.min_comp_size) ||
        (ctx->cfg.max_comp_size && src_sz > ctx->cfg.max_comp_size))
        return 0;                              /* bypass */

    /* 1.  choose dictionary -------------------------------------- */
    uint16_t did = atomic_load_explicit(&ctx->cur_dict_id,
                                        memory_order_acquire);
    const ZSTD_CDict *cd = did ? get_cdict_by_id(did) : NULL;

    /* 2.  prepare TLS scratch ------------------------------------ */
    size_t bound = ZSTD_compressBound(src_sz);
    tls_ensure(bound);                         /* ensure scratch ≥ bound */
    void *dst_buf = tls.scratch;

    /* 3.  compress ----------------------------------------------- */
    size_t csz = cd
        ? ZSTD_compress_usingCDict(tls.cctx, dst_buf, bound,
                                   src, src_sz, cd)
        : ZSTD_compressCCtx      (tls.cctx, dst_buf, bound,
                                   src, src_sz, ctx->cfg.level);

    if (ZSTD_isError(csz))
        return -(ssize_t)ZSTD_getErrorCode(csz);

    /* 4.  ratio check – skip if no benefit ----------------------- */
    if (csz >= src_sz)
        return 0;

    /* 5.  success ------------------------------------------------ */
    *dst         = dst_buf;          /* valid until same thread calls tls_ensure() again */
    *dict_id_out = cd ? did : 0;     /* 0 = no dictionary             */
    return (ssize_t)csz;
}

ssize_t zstd_decompress(const void *src,
        size_t src_sz, void *dst, size_t dst_sz, uint16_t dict_id) {
    zstd_ctx_t *ctx = zstd_ctx_mut();
    /* 0) sanity checks ----------------------------------------------- */
    if (!ctx || !src || src_sz == 0 || !dst || dst_sz <= 0)
        return -EINVAL; /* invalid arguments */
    /* 1) compute output capacity ---------------------------------- */
    /* 2) pick decompression path ---------------------------------- */
    size_t out_sz;
    if (dict_id == 0) {
        out_sz = ZSTD_decompressDCtx(tls.dctx, dst, dst_sz, src,
                src_sz);
    } else {
        const ZSTD_DDict *dict = get_ddict_by_id(dict_id);
        if (!dict)
            return -EINVAL; /* unknown dictionary ID     */

        out_sz = ZSTD_decompress_usingDDict(tls.dctx, dst, dst_sz, src,
                src_sz, dict);
    }

    if (ZSTD_isError(out_sz))
        return -(ssize_t)ZSTD_getErrorCode(out_sz);   /* correct sign */

    if (out_sz > dst_sz)
        return -EOVERFLOW;                            /* caller too small */

    return (ssize_t) out_sz;
}

/* Return values
 *   >0  : decompressed length
 *    0  : either ITEM_ZSTD flag not set  *or*  item is chunked
 *   <0  : negative errno / ZSTD error code
 */
ssize_t zstd_maybe_decompress(const item *it, mc_resp    *resp) {
    zstd_ctx_t *ctx = zstd_ctx_mut();
    if (!ctx || !it || !resp)
        return -EINVAL;                  /* invalid arguments */
    /* 1. Skip if not compressed or chunked ------------------------ */
     if (!(it->it_flags & ITEM_ZSTD) || (it->it_flags & ITEM_CHUNKED))
         return 0;                         /* treat as plain payload */

     /* 2. Dictionary lookup --------------------------------------- */
     uint16_t  did = ITEM_get_dictid(it);
     const ZSTD_DDict *dd = get_ddict_by_id(did);
     if (!dd)
         return -EINVAL;                  /* unknown dict id */

     /* 3. Prepare destination buffer ------------------------------ */
     const void *src     = ITEM_data(it);
     size_t      compLen = it->nbytes;

     size_t expect = ZSTD_getFrameContentSize(src, compLen);
     if (expect == ZSTD_CONTENTSIZE_ERROR)
         return -EINVAL;
     if (expect == ZSTD_CONTENTSIZE_UNKNOWN)
         expect = compLen * 4u;           /* pessimistic */

     void *dst = malloc(expect);
     if (!dst)
         return -ENOMEM;

     /* 4. Decompress ---------------------------------------------- */
     ssize_t dec = zstd_decompress(src, compLen, dst, expect, did);

     if (dec < 0) {                       /* ZSTD error */
         free(dst);
         return dec;
     }

     /* 5. Hand buffer to network layer ---------------------------- */
     resp->write_and_free = dst;
     return dec;                          /* decompressed bytes */
}
