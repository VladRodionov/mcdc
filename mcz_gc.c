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
 * mcz_gc.c
 *
 * Garbage collector (GC) for retired dictionary tables.
 *
 * Responsibilities:
 *   - Manage lifecycle of old mcz_table_t instances published by the trainer
 *     or provided externally.
 *   - Use an MPSC (multi-producer, single-consumer) stack/queue to enqueue
 *     retired tables safely from publisher threads.
 *   - Run a background GC thread that:
 *       • Waits for a configurable "cooling-off" period (default 1h).
 *       • Frees retired tables from memory once no readers can hold references.
 *       • Optionally removes obsolete .dict / .mf files from disk (after "quarantine" period, default is 7d).
 *   - Provide start/stop APIs for GC thread and enqueue APIs for publishers.
 *
 * Design:
 *   - Copy-on-write publishing: readers always see a stable mcz_table_t.
 *   - Old tables are enqueued after publishing a new one.
 *   - GC thread periodically drains queue, checks timestamps, and frees.
 *
 * Naming convention:
 *   - All functions/types prefixed with `mcz_gc_*` belong to this subsystem.
 */
#define _POSIX_C_SOURCE 200809L
#include "mcz_gc.h"

#include <stdatomic.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mcz_utils.h"     /* set_err (if we want), but not required here */
#include "mcz_dict.h"      /* mcz_table_t, mcz_dict_meta_t, mcz_ns_entry_t */
#include "mcz_compression.h" /* for mcz_current_table(ctx) accessor */

/* ---------- Helpers ---------- */

static inline unsigned get_cool(const mcz_ctx_t *ctx) {
    unsigned v = ctx->cfg->gc_cool_period;
    return v ? v : 3600u; /* default 1h */
}

static inline unsigned get_quarantine(const mcz_ctx_t *ctx) {
    unsigned v = ctx->cfg->gc_quarantine_period;
    return v ? v : 3600u * 7 * 24; /* default 7d */
}

static void delete_file_if_dead(const char *path) {
    if (!path || !*path) return;
    (void)unlink(path); /* ignore errors; ENOENT etc. */
}

/* Build "live" check against CURRENT table:
   returns true if (id, path) is present in current table’s by_id with same path. */
static bool is_meta_live_in_current(const mcz_ctx_t *ctx, uint16_t id, const char *dict_path, const char *mf_path) {
    const mcz_table_t *cur = (const mcz_table_t*)atomic_load_explicit(&ctx->dict_table, memory_order_acquire);
    if (!cur) return false;
    const mcz_dict_meta_t *m = cur->by_id[id];
    if (!m) return false;
    /* If a newer meta for same ID reuses the same files, consider them live */
    if (dict_path && m->dict_path && strcmp(dict_path, m->dict_path) == 0) return true;
    if (mf_path   && m->mf_path   && strcmp(mf_path,   m->mf_path)   == 0) return true;
    return false;
}

/* Reverse a singly-linked list; returns new head */
static mcz_retired_node_t* reverse_list(mcz_retired_node_t *h) {
    mcz_retired_node_t *prev = NULL, *cur = h;
    while (cur) { mcz_retired_node_t *n = cur->next; cur->next = prev; prev = cur; cur = n; }
    return prev;
}

/* ---------- Public: enqueue (MPSC push) ---------- */
void mcz_gc_enqueue_retired(mcz_ctx_t *ctx, mcz_table_t *old_tab) {
    if (!old_tab) return;
    mcz_retired_node_t *node = (mcz_retired_node_t*)malloc(sizeof(*node));
    if (!node) return; /* drop on OOM; worst case we leak 'old_tab' until process exit */
    node->tab = old_tab;
    node->retired_at = time(NULL);
    /* MPSC stack push */
    mcz_retired_node_t *head;
    do {
        head = atomic_load_explicit(&ctx->gc_retired_head, memory_order_acquire);
        node->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&ctx->gc_retired_head, &head, node,
                memory_order_release, memory_order_acquire));
}

/* ---------- Free a table deep ---------- */
void mcz_free_table(mcz_table_t *tab) {
    if (!tab) return;
    if (tab->spaces) {
        for (size_t i = 0; i < tab->nspaces; ++i) {
            mcz_ns_entry_t *sp = tab->spaces[i];
            if (!sp) continue;
            free(sp->dicts);
            free(sp->prefix);
            free(sp);
        }
        free(tab->spaces);
    }
    if (tab->metas) {
        for (size_t i = 0; i < tab->nmeta; ++i) {
            mcz_dict_meta_t *m = &tab->metas[i];
            free(m->dict_path);
            free(m->mf_path);
            if (m->prefixes) {
                for (size_t j = 0; j < m->nprefixes; ++j) free(m->prefixes[j]);
                free(m->prefixes);
            }
            free(m->signature);
            /* cdict/ddict are non-owning here */
        }
        free(tab->metas);
    }
    free(tab);
}


/* Final GC pass: free dict objects after cool-off; delete files after quarantine.
 * Assumptions:
 *  - Retirement-time code already dropped pool refs for metas (per-namespace as needed).
 *  - If is_meta_live_in_current(...) == false, no thread can still hold the table/meta.
 *  - get_cool(ctx) returns the cool-off (seconds) for memory frees.
 *  - get_quarantine(ctx) returns FS quarantine (seconds) for file deletion (based on m->retired).
 */
static void gc_process_expired_batch(mcz_ctx_t *ctx, mcz_retired_node_t *batch_head) {
    const time_t now        = time(NULL);
    const unsigned cool     = get_cool(ctx);
    const unsigned quarantine = get_quarantine(ctx);

    mcz_retired_node_t *keep_head = NULL;

    for (mcz_retired_node_t *n = batch_head; n; ) {
        mcz_retired_node_t *next = n->next;
        bool keep_node = false;
        /* Wait for table-level cool-off to avoid TLS/reader races. */
        if ((time_t)cool > 0 && difftime(now, n->retired_at) < (double)cool) {
            keep_node = true;
        } else if (n->tab && n->tab->metas) {
            /* Process each meta in the retired table */
            for (size_t i = 0; i < n->tab->nmeta; ++i) {
                mcz_dict_meta_t *m = &n->tab->metas[i];
                /* If this meta is still live in the current table, defer. */
                if (is_meta_live_in_current(ctx, m->id, m->dict_path, m->mf_path)) {
                    keep_node = true;
                    continue;
                }
                /* ---- Free ZSTD dict objects (safe after cool-off & not-live) ---- */
                if (m->cdict) { ZSTD_freeCDict((ZSTD_CDict*)m->cdict); m->cdict = NULL; }
                if (m->ddict) { ZSTD_freeDDict((ZSTD_DDict*)m->ddict); m->ddict = NULL; }
                /* ---- Delete files after dictionary-level quarantine ----
                 * Use m->retired (time the dict was retired), not the table retirement time.
                 * If not yet retired at dict level, or quarantine not elapsed, defer.
                 */
                if (m->retired == 0 || (quarantine > 0 && difftime(now, m->retired) < (double)quarantine)) {
                    keep_node = true; /* not eligible for deletion yet */
                } else {
                    delete_file_if_dead(m->dict_path);
                    delete_file_if_dead(m->mf_path);
                }
            }
        }
        if (!keep_node) {
            /* All metas fully handled: free table & node */
            mcz_free_table(n->tab);
            free(n);
        } else {
            /* Requeue for a later GC pass */
            n->next = keep_head;
            keep_head = n;
        }
        n = next;
    }

    /* Requeue kept nodes back to the MPSC stack */
    while (keep_head) {
        mcz_retired_node_t *n = keep_head;
        keep_head = keep_head->next;
        n->next = NULL;
        mcz_retired_node_t *head;
        do {
            head = atomic_load_explicit(&ctx->gc_retired_head, memory_order_acquire);
            n->next = head;
        } while (!atomic_compare_exchange_weak_explicit(
                     &ctx->gc_retired_head, &head, n,
                     memory_order_release, memory_order_acquire));
    }
}

static void *gc_thread_main(void *arg) {
    mcz_ctx_t *ctx = (mcz_ctx_t*)arg;
    
    const unsigned min_sleep_ms = 200;     /* quick wake-up for bursts */
    const unsigned max_sleep_ms = 2000;    /* backoff upper bound */
    unsigned cur_sleep_ms = min_sleep_ms;
    
    while (!atomic_load_explicit(&ctx->gc_stop, memory_order_acquire)) {
        /* Drain the MPSC stack in one atomic op */
        mcz_retired_node_t *batch = atomic_exchange_explicit(&ctx->gc_retired_head, NULL, memory_order_acq_rel);
        if (batch) {
            /* We popped a LIFO stack; reverse to process in FIFO-ish order */
            batch = reverse_list(batch);
            gc_process_expired_batch(ctx, batch);
            cur_sleep_ms = min_sleep_ms; /* work found -> reset backoff */
        } else {
            /* No work; backoff sleep */
            struct timespec ts;
            cur_sleep_ms = (cur_sleep_ms < max_sleep_ms) ? (cur_sleep_ms * 2) : max_sleep_ms;
            ts.tv_sec  = cur_sleep_ms / 1000;
            ts.tv_nsec = (cur_sleep_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
    }

    /* Final drain on shutdown */
    mcz_retired_node_t *batch = atomic_exchange_explicit(&ctx->gc_retired_head, NULL, memory_order_acq_rel);
    if (batch) {
        batch = reverse_list(batch);
        gc_process_expired_batch(ctx, batch);
    }
    return NULL;
}

/* ---------- Public: start/stop ---------- */

int mcz_gc_start(mcz_ctx_t *ctx) {
    if (!ctx) return -EINVAL;
    
    atomic_store_explicit(&ctx->gc_stop, false, memory_order_relaxed);
    atomic_store_explicit(&ctx->gc_retired_head, NULL, memory_order_relaxed);

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return -errno;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    int rc = pthread_create(&ctx->gc_tid, &attr, gc_thread_main, ctx);
    pthread_attr_destroy(&attr);
    if (rc != 0) return -rc;
    return 0;
}

void mcz_gc_stop(mcz_ctx_t *ctx) {
    if (!ctx) return;
    atomic_store_explicit(&ctx->gc_stop, true, memory_order_release);
    (void)pthread_join(ctx->gc_tid, NULL);
    /* Ensure nothing left in the queue */
    mcz_retired_node_t *batch = atomic_exchange_explicit(&ctx->gc_retired_head, NULL, memory_order_acq_rel);
    if (batch) {
        batch = reverse_list(batch);
        gc_process_expired_batch(ctx, batch);
    }
}
