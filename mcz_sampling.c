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
 * mcz_sampling.c
 *
 * Single-consumer spooler with an internal MPSC stack and background thread.
 * The thread writes records to <spool_dir>/mcz_samples_YYYYMMDD_HHMMSS.bin
 * until 'spool_max_bytes' is reached, then flips 'g_running' flag to false and stops.
 */

#define _GNU_SOURCE
#include "mcz_sampling.h"
#include "mcz_utils.h"

#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

/* ---------------------- Node & MPSC ---------------------- */

struct full_sample_node_s {
    struct full_sample_node_s *next;
    void   *key;
    size_t  klen;
    void   *val;
    size_t  vlen;
};

/* MPSC head (Treiber stack) */
static _Atomic(full_sample_node_t *) g_head = NULL;

/* Push a node onto the MPSC stack */
static inline void mpsc_push(full_sample_node_t *node) {
    full_sample_node_t *old_head;
    do {
        old_head   = atomic_load_explicit(&g_head, memory_order_acquire);
        node->next = old_head;
    } while (!atomic_compare_exchange_weak_explicit(
                 &g_head, &old_head, node,
                 memory_order_release, memory_order_relaxed));
}

/* Drain: take the whole stack and return it (LIFO order) */
static inline full_sample_node_t *mpsc_drain(void) {
    return atomic_exchange_explicit(&g_head, NULL, memory_order_acq_rel);
}
static inline full_sample_node_t *reverse_list(full_sample_node_t *h) {
    full_sample_node_t *p = NULL, *c = h;
    while (c) { full_sample_node_t *n = c->next; c->next = p; p = c; c = n; }
    return p;
}

/* ---------------------- Config/State ---------------------- */

typedef struct {
    char   *spool_dir;        /* owned copy */
    double  sample_p;         /* 0..1 */
    int sample_window_sec;
    size_t  spool_max_bytes;        /* file cap */
} sampler_cfg_t;

static sampler_cfg_t g_cfg        = {0};
static _Atomic bool  g_configured = false;
static _Atomic bool  g_running    = false;     /* thread running */
static _Atomic size_t g_written   = 0;
static _Atomic size_t g_collected = 0;


static pthread_t g_thr;
static char      g_path[1024] = {0};


/* ---------------------- IO helpers ---------------------- */

static inline void u32le(uint32_t v, unsigned char out[4]) {
    out[0] = (unsigned char)(v & 0xFF);
    out[1] = (unsigned char)((v >> 8) & 0xFF);
    out[2] = (unsigned char)((v >> 16) & 0xFF);
    out[3] = (unsigned char)((v >> 24) & 0xFF);
}

static int write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

static int make_path(char *dst, size_t cap, const char *dir, time_t t) {
    if (!dst || cap == 0) return -EINVAL;
    memset(dst, 0, cap);   // <-- zero it first
    struct tm tmv;
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    localtime_r(&t, &tmv);
#else
    tmv = *localtime(&t);
#endif
    if (!dir || !*dir) dir = ".";
    int n = snprintf(dst, cap, "%s/mcz_samples_%04d%02d%02d_%02d%02d%02d.bin",
                     dir,
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}
/* -------- Buffered writer to reduce write() syscalls -------- */

typedef struct {
    int     fd;
    uint8_t *buf;
    size_t  cap;
    size_t  used;
} bufw_t;

static int bufw_init(bufw_t *w, int fd, size_t cap) {
    if (!w) return -EINVAL;
    w->fd = fd;
    w->buf = (uint8_t*)malloc(cap);
    if (!w->buf) return -ENOMEM;
    w->cap = cap;
    w->used = 0;
    return 0;
}

static int bufw_flush(bufw_t *w) {
    if (!w || w->fd < 0) return -EINVAL;
    size_t to_write = w->used;
    const uint8_t *p = w->buf;
    while (to_write) {
        ssize_t n = write(w->fd, p, to_write);
        if (n < 0) {
            if (errno == EINTR) continue;
            return n;
        }
        p += (size_t)n;
        to_write -= (size_t)n;
    }
    w->used = 0;
    return 0;
}

static int bufw_write(bufw_t *w, const void *data, size_t len) {
    if (!w || w->fd < 0) return -EINVAL;
    const uint8_t *p = (const uint8_t*)data;
    /* Fast path: fits into buffer */
    if (len <= (w->cap - w->used)) {
        memcpy(w->buf + w->used, p, len);
        w->used += len;
        return 0;
    }
    /* Fill, flush, then if large chunk, write direct, else buffer remainder */
    if (w->used && bufw_flush(w) != 0) return -1;
    if (len >= w->cap) {
        /* Too large: write through */
        return write_all(w->fd, data, len);
    }
    memcpy(w->buf, p, len);
    w->used = len;
    return 0;
}

static void bufw_destroy(bufw_t *w) {
    if (!w) return;
    if (w->buf) { (void)bufw_flush(w); free(w->buf); }
    w->buf = NULL; w->cap = w->used = 0; w->fd = -1;
}
/* ---------------------- Thread ---------------------- */

static void free_list(full_sample_node_t *n) {
    while (n) {
        full_sample_node_t *nx = n->next;
        if (n->key) free(n->key);
        if (n->val) free(n->val);
        free(n);
        n = nx;
    }
}

static void *sampler_main(void *arg) {
    (void)arg;
    
    /* Ensure directory exists (best-effort) */
    if (g_cfg.spool_dir && *g_cfg.spool_dir) {
        if (mkdir(g_cfg.spool_dir, 0777) != 0 && errno != EEXIST) {
            atomic_store_explicit(&g_running, false, memory_order_release);
            return NULL;
        }
    }
    
    /* Open file */
    time_t start = time(NULL);
    if (make_path(g_path, sizeof(g_path), g_cfg.spool_dir, start) != 0) {
        g_path[0] = '\0';
        atomic_store_explicit(&g_running, false, memory_order_release);
        return NULL;
    }
    int fd = open(g_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) {
        g_path[0] = '\0';
        atomic_store_explicit(&g_running, false, memory_order_release);
        return NULL;
    }
    
    /* Buffered writer */
    bufw_t bw;
    if (bufw_init(&bw, fd, 1u << 20) != 0) {
        close(fd);
        g_path[0] = '\0';
        atomic_store_explicit(&g_running, false, memory_order_release);
        return NULL;
    }
    
    atomic_store_explicit(&g_written, 0, memory_order_release);
    
    const size_t cap = g_cfg.spool_max_bytes ? g_cfg.spool_max_bytes : ((size_t)64 * 1024 * 1024);
    const struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 }; /* 10ms */
    full_sample_node_t *lst = NULL, *cur = NULL;
    
    bool time_limit_enabled = g_cfg.sample_window_sec > 0;
    while (atomic_load_explicit(&g_running, memory_order_acquire)) {
        cur = NULL; lst = NULL;
        if (time_limit_enabled) {
            time_t _now = time(NULL);
            if ((int)difftime(_now, start) >= g_cfg.sample_window_sec) {
                goto stop;
            }
        }
        lst = mpsc_drain();
        if (!lst) { nanosleep(&ts, NULL); continue; }
        
        lst = reverse_list(lst);
        for (cur = lst; cur; ) {
            if (cur->klen <= UINT32_MAX && cur->vlen <= UINT32_MAX) {
                unsigned char hdr[8];
                u32le((uint32_t)cur->klen, hdr + 0);
                u32le((uint32_t)cur->vlen, hdr + 4);
                if (bufw_write(&bw, hdr, 8) != 0) {
                    goto io_error;
                }
                if (cur->klen && bufw_write(&bw, cur->key, cur->klen) != 0) {
                    goto io_error;
                }
                if (cur->vlen && bufw_write(&bw, cur->val, cur->vlen) != 0) {
                    goto io_error;
                }
                
                size_t inc = 8 + cur->klen + cur->vlen;
                size_t total = atomic_fetch_add_explicit(&g_written, inc, memory_order_acq_rel) + inc;
                if (total >= cap) {
                    goto stop;
                }
            }
            full_sample_node_t *nx = cur->next;
            if (cur->key) free(cur->key);
            if (cur->val) free(cur->val);
            free(cur);
            cur = nx;
        }
    }
    
    (void)bufw_flush(&bw);
    bufw_destroy(&bw);
    if (fd >= 0) { close(fd); fd = -1; }
    return NULL;


io_error:
    perror("mcz:sampler write");
stop:
    (void)bufw_flush(&bw);
    bufw_destroy(&bw);
    if (fd >= 0) { close(fd); fd = -1; }
    
    atomic_store_explicit(&g_collected, 0, memory_order_release);
    atomic_store_explicit(&g_running, false, memory_order_release);
    /* free leftovers from current batch */
    while (cur) {
        full_sample_node_t *nx = cur->next;
        if (cur->key) free(cur->key);
        if (cur->val) free(cur->val);
        free(cur);
        cur = nx;
    }
    return NULL;
}
/* ---------------------- Public API ---------------------- */

int mcz_sampler_init(const char *spool_dir,
                     double sample_p,
                     int sample_window_sec,
                     size_t spool_max_bytes) {
    g_cfg.spool_dir = (char *) spool_dir;
    g_cfg.sample_p = sample_p;
    g_cfg.spool_max_bytes = spool_max_bytes;
    g_cfg.sample_window_sec = sample_window_sec;
    atomic_store_explicit(&g_configured, true, memory_order_release);
    return 0;
}

int mcz_sampler_start(void)
{
    if (!atomic_load_explicit(&g_configured, memory_order_acquire))
        return -EINVAL;

    bool expect = false;
    if (!atomic_compare_exchange_strong_explicit(&g_running, &expect, true,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        return 1; /* already running */
    }
    int rc = pthread_create(&g_thr, NULL, sampler_main, NULL);
    if (rc != 0) {
        atomic_store_explicit(&g_running, false, memory_order_release);
        return -rc;
    }
    return 0;
}

int mcz_sampler_stop(void)
{
    if (!atomic_load_explicit(&g_running, memory_order_acquire))
        return 1; /* not running */
    atomic_store_explicit(&g_running, false, memory_order_release);
    (void)pthread_join(g_thr, NULL);
    return 0;
}

int mcz_sampler_maybe_record(const void *key, size_t klen,
                             const void *val, size_t vlen)
{
    if (!atomic_load_explicit(&g_configured, memory_order_acquire))
        return -EINVAL;

    if (!atomic_load_explicit(&g_running, memory_order_acquire))
        return 0; /* not collecting */

    double p = g_cfg.sample_p;
    if (p <= 0.0) return 0;
    if (p < 1.0) {
        if (p > 1.0) p = 1.0;
        uint32_t threshold = (uint32_t)((double)UINT32_MAX * p);
        if (fast_rand32() > threshold) return 0;
    }
    size_t collected = atomic_load_explicit(&g_collected, memory_order_acquire);
    
    if (collected >= g_cfg.spool_max_bytes) {
        return 0;
    }
    full_sample_node_t *n = (full_sample_node_t *)malloc(sizeof(*n));
    if (!n) return -ENOMEM;

    void *k = NULL, *v = NULL;
    if (klen) { k = malloc(klen); if (!k) { free(n); return -ENOMEM; } memcpy(k, key, klen); }
    if (vlen) { v = malloc(vlen); if (!v) { free(k); free(n); return -ENOMEM; } memcpy(v, val, vlen); }

    n->next = NULL;
    n->key  = k;  n->klen = klen;
    n->val  = v;  n->vlen = vlen;

    mpsc_push(n);
    atomic_fetch_add_explicit(&g_collected, klen + vlen + 8, memory_order_acq_rel);
    return 1;
}

/* Get a status snapshot. 'out' must be non-NULL. */
void mcz_sampler_get_status(mcz_sampler_status_t *out)
{
    if (!out) return;

    bool running = atomic_load_explicit(&g_running, memory_order_acquire);
    out->configured    = atomic_load_explicit(&g_configured, memory_order_acquire);
    out->running       = running;
    out->bytes_written = atomic_load_explicit(&g_written, memory_order_acquire);
    out->bytes_collected = atomic_load_explicit(&g_collected, memory_order_acquire);

    size_t i = 0;
    for (; i + 1 < sizeof(out->current_path) && g_path[i]; ++i)
        out->current_path[i] = g_path[i];
    out->current_path[i] = '\0';
}
/* For tests/shutdown: drain and free queued items without writing to disk. */
void mcz_sampler_drain_queue(void)
{
    full_sample_node_t *lst = mpsc_drain();
    if (!lst) return;
    free_list(lst);
}

