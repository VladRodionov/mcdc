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
 * mcz_dict_pool.c
 *
 * Global registry (pool) for compiled dictionaries (ZSTD_CDict / ZSTD_DDict).
 *
 * Responsibilities:
 *   - Provide a central reference-counted store for dictionaries.
 *   - Avoid double-loading or premature free when the same dict is shared
 *     across multiple namespaces or routing tables.
 *   - APIs to:
 *       • Retain dictionaries when publishing new tables.
 *       • Release dictionaries when tables are garbage-collected.
 *       • Lookup by unique key (e.g., signature or file path).
 *
 * Design:
 *   - Thread-safe, typically protected by a mutex.
 *   - Each pool entry maintains its own refcount.
 *   - Freed only when refcount drops to zero.
 *
 * Naming convention:
 *   - All functions/types prefixed with `mcz_dict_pool_*` belong here.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   // for O_CLOEXEC on glibc + some GNU bits
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L  // for setenv(), unsetenv(), realpath(), etc.
#endif

#include "mcz_dict_pool.h"
#include "mcz_utils.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>


typedef struct pool_entry_s {
    struct pool_entry_s *next;
    char                *key;     /* prefers mf signature; else dict_path */
    const ZSTD_CDict    *cdict;
    const ZSTD_DDict    *ddict;
    _Atomic uint32_t     refcnt;
} pool_entry_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pool_entry_t   *g_head = NULL;

char *make_key_from_meta(const mcz_dict_meta_t *m) {
    /* Prefer a strong identity if we have it (signature),
       otherwise fall back to absolute dict_path. */
    if (m->signature && *m->signature) return strdup(m->signature);
    if (m->dict_path && *m->dict_path) return strdup(m->dict_path);
    return NULL; /* should not happen for valid metas */
}

int mcz_dict_pool_init(void) {
    g_head = NULL;
    if (pthread_mutex_init(&g_lock, NULL) != 0) return -1;
    return 0;
}

void mcz_dict_pool_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    pool_entry_t *e = g_head;
    g_head = NULL;
    pthread_mutex_unlock(&g_lock);

    /* Free outside the lock */
    while (e) {
        pool_entry_t *n = e->next;
        if (e->cdict) ZSTD_freeCDict((ZSTD_CDict*)e->cdict);
        if (e->ddict) ZSTD_freeDDict((ZSTD_DDict*)e->ddict);
        free(e->key);
        free(e);
        e = n;
    }
    pthread_mutex_destroy(&g_lock);
}

/* Find by key (under lock) */
static pool_entry_t *find_locked(const char *key) {
    if (!g_head) return NULL;
    for (pool_entry_t *p = g_head; p; p = p->next)
        if (strcmp(p->key, key) == 0) return p;
    return NULL;
}


/* Add pool entry only if it does not exist.
 * - Uses meta->signature (preferred) or meta->dict_path as the key (via make_key_from_meta()).
 * - Initial refcount = number of namespaces (>=1).
 * - If entry already exists, do NOT increment (avoid double-counting).
 * - if entry exists, we deallocate dicts and reuse existing from an entry (side effect)
 * Returns 0 on success, <0 on error.
 */
int mcz_dict_pool_retain_for_meta(mcz_dict_meta_t *m, char **err_out)
{
    if (!m) {
        set_err(err_out, "dict_pool: NULL meta passed to retain");
        return -1;
    }

    /* build key string for this meta (signature preferred, else path) */
    char *key = make_key_from_meta(m);
    if (!key) {
        set_err(err_out, "dict_pool: failed to allocate key for meta id=%u", m->id);
        return -1;
    }

    /* refcount increment (only applied if new entry is created) */
    uint32_t inc = (m->nprefixes && m->prefixes) ? (uint32_t)m->nprefixes : 1u;

    pthread_mutex_lock(&g_lock);

    pool_entry_t *e = find_locked(key);
    if (e) {
        /* deallocate dicts and reuse existing ones */
        if (m->cdict) ZSTD_freeCDict((ZSTD_CDict*)m->cdict);
        if (m->ddict) ZSTD_freeDDict((ZSTD_DDict*)m->ddict);
        m->cdict = e->cdict;
        m->ddict = e->ddict;
        /* already present, nothing to do */
        pthread_mutex_unlock(&g_lock);
        free(key);
        return 0;
    }

    /* not found: must have valid compiled dicts in meta */
    if (!m->cdict || !m->ddict) {
        pthread_mutex_unlock(&g_lock);
        set_err(err_out, "dict_pool: missing compiled dicts for %s",
                m->dict_path ? m->dict_path : "(unknown)");
        free(key);
        return -1;
    }

    e = (pool_entry_t*)calloc(1, sizeof(*e));
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        set_err(err_out, "dict_pool: OOM allocating pool entry for %s", key);
        free(key);
        return -1;
    }

    e->key   = key;          /* ownership transferred */
    e->cdict = m->cdict;
    e->ddict = m->ddict;
    atomic_store_explicit(&e->refcnt, inc, memory_order_relaxed);

    /* push new entry into global list */
    e->next = g_head;
    g_head  = e;

    pthread_mutex_unlock(&g_lock);
    return 0;
}

void mcz_dict_pool_release_for_meta(const mcz_dict_meta_t *m, int32_t *ref_left, char **err_out) {
    if (!m) return;
    char *key = make_key_from_meta(m);
    if (!key) return;

    pthread_mutex_lock(&g_lock);
    pool_entry_t *prev = NULL, *e = g_head;
    while (e && strcmp(e->key, key) != 0) { prev = e; e = e->next; }

    if (e) {
        uint32_t left = atomic_fetch_sub_explicit(&e->refcnt, 1, memory_order_acq_rel) - 1;
        *ref_left = left;
        if (left == 0) {
            /* unlink */
            if (prev) prev->next = e->next; else g_head = e->next;
            pthread_mutex_unlock(&g_lock);
            free(e->key);
            free(e);
            free(key);
            return;
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(key);
}

int mcz_dict_pool_refcount_for_meta(const mcz_dict_meta_t *meta)
{
    if (!meta) return -1;
    const char *key = (meta->signature && *meta->signature)
                        ? meta->signature
                        : meta->dict_path;
    if (!key) return -1;

    pthread_mutex_lock(&g_lock);

    pool_entry_t *cur = g_head;
    while (cur) {
        const char *ekey = cur->key;
        if (ekey && strcmp(ekey, key) == 0) {
            int rc = atomic_load_explicit(&cur->refcnt, memory_order_relaxed);
            pthread_mutex_unlock(&g_lock);
            return rc;
        }
        cur = cur->next;
    }

    pthread_mutex_unlock(&g_lock);
    return -1;
}

void mcz_dict_pool_dump(FILE *out)
{
    if (!out) out = stdout;

    pthread_mutex_lock(&g_lock);

    fprintf(out, "---- MCZ Dictionary Pool Dump ----\n");

    pool_entry_t *cur = g_head;
    size_t n = 0;
    while (cur) {
        int rc = atomic_load_explicit(&cur->refcnt, memory_order_relaxed);
        fprintf(out, " [%zu] key=\"%s\" refcount=%d cdict=%p ddict=%p\n",
                n,
                cur->key ? cur->key : "(null)",
                rc,
                (void*)cur->cdict,
                (void*)cur->ddict);
        cur = cur->next;
        n++;
    }

    fprintf(out, " Total entries: %zu\n", n);
    fprintf(out, "----------------------------------\n");

    pthread_mutex_unlock(&g_lock);
}
