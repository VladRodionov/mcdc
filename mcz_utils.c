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
 * mcz_utils.c
 *
 * Common utility helpers shared across the memcached-Zstd (MCZ) modules.
 *
 * Responsibilities:
 *   - Error handling helpers (set_err).
 *   - File I/O helpers (atomic_write_file, atomic_write_text).
 *   - Time formatting (RFC3339 UTC).
 *   - Directory fsync after renames.
 *   - String manipulation (namespace joiners, etc.).
 *
 * Design:
 *   - Standalone, no dependencies on higher-level MCZ modules.
 *   - Intended for small, reusable building blocks.
 *   - All helpers prefixed with `mcz_` for consistency.
 */
#include "mcz_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/time.h>

// Thread-local state (must be seeded once per thread)
static __thread uint32_t rnd_state = 2463534242u; // arbitrary nonzero seed

uint32_t fast_rand32(void) {
    uint32_t x = rnd_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rnd_state = x;
    return x;
}

/* Portable timegm fallback (assumes tm is UTC) */
static inline time_t timegm_fallback(struct tm *tm) {
    char *old = getenv("TZ");
    if (old) old = strdup(old);
    setenv("TZ", "UTC", 1); tzset();
    time_t t = mktime(tm);
    if (old) { setenv("TZ", old, 1); free(old); } else { unsetenv("TZ"); }
    tzset();
    return t;
}


/* ---------- small utils ---------- */
inline char *xstrdup(const char *s){ return s?strdup(s):NULL; }

inline void trim(char *s){
    if (!s) return;
    char *p=s;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t n=strlen(p);
    memmove(s,p,n+1);
    if (!n) return;
    char *e = s + n - 1;
    while (e>=s && isspace((unsigned char)*e)) *e--='\0';
}

inline int join_path(char *dst, size_t cap, const char *dir, const char *file){
    if (!dir || !file) return -EINVAL;
    size_t nd=strlen(dir), nf=strlen(file);
    if (nd+1+nf+1 > cap) return -ENAMETOOLONG;
    strcpy(dst, dir);
    if (nd && dst[nd-1] != '/') dst[nd++] = '/';
    strcpy(dst+nd, file);
    return 0;
}
#if !defined(HAVE_TIMEGM)
/* Provide a fallback if the platform lacks timegm(3). */
#  define timegm timegm_fallback
#endif

/* Parse RFC3339/ISO-8601 UTC "YYYY-MM-DDTHH:MM:SSZ" (Phase-1 minimal) */
int parse_rfc3339_utc(const char *s, time_t *out){
    if (!s) return -EINVAL;
    int Y,M,D,h,m; int S;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ", &Y,&M,&D,&h,&m,&S) != 6) return -EINVAL;
    struct tm tm = {0};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = S;

    time_t t = timegm(&tm);

    if (t == (time_t)-1) return -EINVAL;
    *out = t; return 0;
}

/* Split comma-separated namespaces into array */
int split_prefixes(char *csv, char ***out, size_t *nout){
    size_t cap=4, n=0;
    char **arr = calloc(cap, sizeof(*arr));
    if (!arr) return -ENOMEM;
    for (char *tok = strtok(csv, ","); tok; tok = strtok(NULL, ",")) {
        trim(tok);
        if (!*tok) continue;
        if (n==cap){ cap*=2; char **tmp=realloc(arr, cap*sizeof(*arr)); if(!tmp){ free(arr); return -ENOMEM; } arr=tmp; }
        arr[n++] = xstrdup(tok);
    }
    *out = arr; *nout = n;
    return 0;
}
/* Helper: set error string if provided */
void set_err(char **err_out, const char *fmt, ...) {
    if (!err_out) return;

    va_list ap;
    va_start(ap, fmt);

    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    va_end(ap);

    *err_out = strdup(buf);
}

/* RFC3339 UTC formatter: "YYYY-MM-DDTHH:MM:SSZ" */
void format_rfc3339_utc(time_t t, char out[32]) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* fsync a directory path (best-effort) */
int fsync_dirpath(const char *dirpath) {
    int dfd = open(dirpath, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return -errno;

    int rc = 0;
    if (fsync(dfd) != 0)
        rc = -errno;

    close(dfd);
    return rc;
}

/* Write data to tmp file + fsync + rename to final_path. */
int atomic_write_file(const char *dir, const char *final_path,
                      const void *data, size_t len, mode_t mode,
                      char **err_out)
{
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", final_path, (int)getpid());

    int fd = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, mode);
    if (fd < 0) {
        set_err(err_out, "open(%s): %s", tmp_path, strerror(errno));
        return -errno;
    }

    const unsigned char *p = (const unsigned char *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            int e = -errno;
            set_err(err_out, "write(%s): %s", tmp_path, strerror(errno));
            close(fd);
            unlink(tmp_path);
            return e;
        }
        off += (size_t)w;
    }

    if (fsync(fd) != 0) {
        int e = -errno;
        set_err(err_out, "fsync(%s): %s", tmp_path, strerror(errno));
        close(fd);
        unlink(tmp_path);
        return e;
    }
    if (close(fd) != 0) {
        int e = -errno;
        set_err(err_out, "close(%s): %s", tmp_path, strerror(errno));
        unlink(tmp_path);
        return e;
    }

    if (rename(tmp_path, final_path) != 0) {
        int e = -errno;
        set_err(err_out, "rename(%s -> %s): %s", tmp_path, final_path, strerror(errno));
        unlink(tmp_path);
        return e;
    }

    (void)fsync_dirpath(dir);
    return 0;
}

/* Convenience: write text (null-terminated) atomically */
int atomic_write_text(const char *dir, const char *final_path,
                      const char *text, char **err_out)
{
    return atomic_write_file(dir, final_path,
                             text, strlen(text), 0644, err_out);
}

char *mcz_join_namespaces(const char * const *prefixes, size_t nprefixes,
                          const char *sep)
{
    if (!sep) sep = ", ";

    /* default if none provided */
    if (!prefixes || nprefixes == 0) {
        char *s = strdup("default");
        return s; /* may be NULL on OOM */
    }

    /* precompute length */
    size_t total = 0;
    for (size_t i = 0; i < nprefixes; ++i) {
        const char *p = prefixes[i] ? prefixes[i] : "";
        total += strlen(p);
        if (i + 1 < nprefixes) total += strlen(sep);
    }

    char *out = (char *)malloc(total + 1);
    if (!out) return NULL;

    out[0] = '\0';
    for (size_t i = 0; i < nprefixes; ++i) {
        const char *p = prefixes[i] ? prefixes[i] : "";
        strcat(out, p);
        if (i + 1 < nprefixes) strcat(out, sep);
    }
    return out;
}

/* ----------------------------------------------------------------------
 * log_rate_limited()
 *   Prints to stderr at most once every `interval_us` micro-seconds.
 *   Uses a static timestamp; thread-safe under POSIX (atomic exchange).
 * -------------------------------------------------------------------- */

uint64_t now_usec(void) /* monotonic wall-clock */
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

void log_rate_limited(uint64_t interval_us, const char *fmt, ...) {

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

int str_to_u16(const char *s, uint16_t *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v <= 0 || v > 0xFFFF)
        return -EINVAL;
    *out = (uint16_t) v;
    return 0;
}

/* RFC 4122 UUID v4 string: 36 chars + NUL (8-4-4-4-12) */
int uuidv4_string(char out[37]) {
    unsigned char r[16];
    FILE *fp = fopen("/dev/urandom", "rb");
    if (!fp) return -1;
    size_t n = fread(r, 1, sizeof(r), fp);
    fclose(fp);
    if (n != sizeof(r)) return -1;

    /* Set version (4) and variant (10x) bits */
    r[6] = (unsigned char)((r[6] & 0x0F) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3F) | 0x80);

    static const char *hex = "0123456789abcdef";
    int p = 0, i = 0;
    for (; i < 16; ++i) {
        out[p++] = hex[(r[i] >> 4) & 0xF];
        out[p++] = hex[r[i] & 0xF];
        if (i==3 || i==5 || i==7 || i==9) out[p++] = '-';
    }
    out[36] = '\0';
    return 0;
}

/* Build "<uuid>.<ext>" into 'out' (PATH-safe join done by caller). */
int make_uuid_basename(const char *ext, char out[64], char **err_out) {
    if (!ext || !*ext) { set_err(err_out, "uuid: empty extension"); return -EINVAL; }
    char u[37];
    if (uuidv4_string(u) != 0) { set_err(err_out, "uuid: generation failed"); return -EIO; }
    int n = snprintf(out, 64, "%s.%s", u, ext);
    if (n <= 0 || n >= 64) { set_err(err_out, "uuid: basename overflow"); return -EOVERFLOW; }
    return 0;
}

inline uint64_t fnv1a64(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

/* allocate zeroed, or return NULL */
inline void *xzmalloc(size_t n) { void *p = calloc(1, n); return p; }

// Increment an _Atomic uint32_t by delta, return the new value
inline uint32_t
atomic_inc32(_Atomic uint32_t *p, uint32_t delta) {
    // fetch_add returns the *old* value; add delta to get the new
    return atomic_fetch_add_explicit(p, delta, memory_order_relaxed) + delta;
}

// Increment an _Atomic uint64_t by delta, return the new value
inline uint64_t
atomic_inc64(_Atomic uint64_t *p, uint64_t delta) {
    return atomic_fetch_add_explicit(p, delta, memory_order_relaxed) + delta;
}

// Increment an _Atomic int64_t by delta, return the new value
inline int64_t
atomic_inc64s(_Atomic int64_t *p, int64_t delta) {
    return atomic_fetch_add_explicit(p, delta, memory_order_relaxed) + delta;
}

/* --------- 32-bit helpers --------- */

/* Read an _Atomic uint32_t */
inline uint32_t
atomic_get32(const _Atomic uint32_t *p) {
    return atomic_load_explicit(p, memory_order_relaxed);
}

/* Set an _Atomic uint32_t */
inline void
atomic_set32(_Atomic uint32_t *p, uint32_t v) {
    atomic_store_explicit(p, v, memory_order_relaxed);
}

/* --------- 64-bit helpers --------- */

/* Read an _Atomic uint64_t */
inline uint64_t
atomic_get64(const _Atomic uint64_t *p) {
    return atomic_load_explicit(p, memory_order_relaxed);
}

/* Set an _Atomic uint64_t */
inline void
atomic_set64(_Atomic uint64_t *p, uint64_t v) {
    atomic_store_explicit(p, v, memory_order_relaxed);
}

/* Read an _Atomic int64_t */
inline int64_t
atomic_get64s(const _Atomic int64_t *p) {
    return atomic_load_explicit(p, memory_order_relaxed);
}

/* Set an _Atomic int64_t */
inline void
atomic_set64s(_Atomic int64_t *p, int64_t v) {
    atomic_store_explicit(p, v, memory_order_relaxed);
}

