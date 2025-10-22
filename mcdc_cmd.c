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
 * mcdc_cmd.c - Implementation of MC/DC command extensions for memcached.
 *
 * This file adds support for custom ASCII commands:
 *
 *   - "mcdc stats [<namespace>|global|default] [json]"
 *       Dump statistics snapshots in text or JSON form.
 *
 *   - "mcdc ns"
 *       List active namespaces, including "global" and "default".
 *
 *   - "mcdc config [json]"
 *       Show current configuration, either as text lines or as a JSON object.
 *
 *   - "mcdc sampler [start|stop|status]"
 *       Sampler control (spooling data, incoming key-value pairs to a file
 *           for further analysis and dictionary creation).
 *   - "mcdc reload [json]"
 *       Reload dictionaries online
 *
 * Each handler builds the appropriate payload (text lines or JSON),
 * attaches it to a memcached response, and writes it back through
 * the standard connection send path.
 *
 * Thread safety:
 *   Commands only read from stats and configuration structures.
 *   Updates are synchronized in other modules; this code assumes
 *   point-in-time snapshots are safe to serialize.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>

#include <stdlib.h>
#include <arpa/inet.h>

#include "mcdc_cmd.h"
#include "mcdc_config.h"

#include "mcdc_sampling.h"
#include "proto_bin.h"

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1


/* Map train mode to string */
static const char *train_mode_str(mcdc_train_mode_t m) {
    switch (m) {
    case MCDC_TRAIN_FAST:     return "FAST";
    case MCDC_TRAIN_OPTIMIZE: return "OPTIMIZE";
    default:                 return "UNKNOWN";
    }
}

static inline const char *b2s(bool v) { return v ? "true" : "false"; }


/* ---------- mcdc sampler status ---------- */
static int sampler_status_ascii(char *buf, size_t cap, mcdc_sampler_status_t *st) {
    int n = snprintf(buf, cap,
        "MCDC-SAMPLER configured %s\r\n"
        "MCDC-SAMPLER running %s\r\n"
        "MCDC-SAMPLER bytes_written %" PRIu64 "\r\n"
        "MCDC-SAMPLER bytes_collected %" PRIu64 ",\r\n"
        "MCDC-SAMPLER path %s\r\n"
        "END\r\n",
        st->configured ? "true" : "false",
        st->running    ? "true" : "false",
        (uint64_t)st->bytes_written,
        (uint64_t)st->bytes_collected,
        st->current_path[0] ? st->current_path : ""
    );
    return n;
}

static int sampler_status_json(char *buf, size_t cap, mcdc_sampler_status_t *st){
    int n = snprintf(buf, cap,
        "{\r\n"
          "\"configured\": %s,\r\n"
          "\"running\": %s,\r\n"
          "\"bytes_written\": %" PRIu64 ",\r\n"
          "\"queue_collected\": %" PRIu64 ",\r\n"
          "\"path\": \"%s\"\r\n"
        "}\r\n",
        st->configured ? "true" : "false",
        st->running    ? "true" : "false",
        (uint64_t)st->bytes_written,
        (uint64_t)st->bytes_collected,
        st->current_path[0] ? st->current_path : ""
    );
    return n;
}

static int build_sampler_status(char **outp, size_t *lenp, int json) {
    mcdc_sampler_status_t st;
    mcdc_sampler_get_status(&st);
    size_t cap = 2048;
    char *buf = (char*)malloc(cap);
    if (!buf) return -1;
    int n = json? sampler_status_json(buf, cap, &st): sampler_status_ascii(buf, cap, &st);
    if (n < 0) { free(buf); return -1; }

    if ((size_t)n >= cap) {
        cap = (size_t)n + 1;
        char *nb = (char*)realloc(buf, cap);
        if (!nb) { free(buf); return -1; }
        buf = nb;
        int n = json? sampler_status_json(buf, cap, &st): sampler_status_ascii(buf, cap, &st);
        if (n < 0) { free(buf); return -1; }
    }
    *outp = buf; *lenp = (size_t)n;
    return 0;
}

/* ---------- ASCII: mcdc sampler ... ---------- */
static void handle_mcdc_sampler_ascii(conn *c, token_t *tokens, size_t ntokens) {
    if (ntokens < 4 || ntokens > 5) { out_string(c, "CLIENT_ERROR usage: mcdc sampler <start|stop|status> [json]"); return; }
    const char *verb = tokens[COMMAND_TOKEN + 2].value;

    if (strcmp(verb, "start") == 0) {
        if (ntokens != 4) { out_string(c, "CLIENT_ERROR usage: mcdc sampler <start|stop|status> [json]"); return; }
        int rc = mcdc_sampler_start();
        if (rc == 0) {
            out_string(c, "STARTED\r\n");
        } else if (rc == 1) {
            out_string(c, "RUNNING\r\n");
        } else {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "SERVER_ERROR sampler_start rc=%d", rc);
            out_string(c, tmp);
        }
        return;
    } else if (strcmp(verb, "stop") == 0) {
        if (ntokens != 4) { out_string(c, "CLIENT_ERROR usage: mcdc sampler <start|stop|status> [json]"); return; }

        int rc = mcdc_sampler_stop();
        if (rc == 0) {
            out_string(c, "STOPPED\r\n");
        } else if (rc == 1) {
            out_string(c, "NOT RUNNING\r\n");
        } else {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "SERVER_ERROR sampler_stop rc=%d", rc);
            out_string(c, tmp);
        }
        return;
    } else if (strcmp(verb, "status") == 0) {
        int want_json = 0;
        if (ntokens == 5 && tokens[COMMAND_TOKEN + 3].value &&
            strcmp(tokens[COMMAND_TOKEN + 3].value, "json") == 0) {
            want_json = 1;
        } else if (ntokens == 5){
            out_string(c, "CLIENT_ERROR usage: mcdc sampler <start|stop|status> [json]");
            return;
        }
        char *payload = NULL; size_t plen = 0;
        int rc = build_sampler_status(&payload, &plen, want_json);
        if (rc != 0 || !payload) { out_string(c, "SERVER_ERROR sampler_status"); return; }
        write_and_free(c, payload, plen);
        return;
    }

    out_string(c, "CLIENT_ERROR usage: mcdc sampler <start|stop|status> [json]");
}

/* ----------  mcdc reload ... ------------*/

static int reload_status_ascii(char *buf, size_t cap, mcdc_reload_status_t *st) {
    int n;
    if (st->rc == 0) {
        n = snprintf(buf, cap,
            "MCDC-RELOAD status OK\r\n"
            "MCDC-RELOAD ns %u\r\n"
            "MCDC-RELOAD dicts_loaded %u\r\n"
            "MCDC-RELOAD dicts_new %u\r\n"
            "MCDC-RELOAD dicts_reused %u\r\n"
            "MCDC-RELOAD dicts_failed %u\r\n"
            "END\r\n",
            st->namespaces, st->dicts_loaded, st->dicts_new, st->dicts_reused, st->dicts_failed);
    } else if (st->err[0]) {
        n = snprintf(buf, cap,
            "MCDC-RELOAD status ERR\r\n"
            "MCDC-RELOAD rc %d\r\n"
            "MCDC-RELOAD msg %s\r\n"
            "MCDC-RELOAD ns %u\r\n"
            "MCDC-RELOAD dicts_loaded %u\r\n"
            "MCDC-RELOAD dicts_new %u\r\n"
            "MCDC-RELOAD dicts_reused %u\r\n"
            "MCDC-RELOAD dicts_failed %u\r\n"
            "END\r\n",
            st->rc, st->err, st->namespaces, st->dicts_loaded, st->dicts_new, st->dicts_reused, st->dicts_failed);
    } else {
        n = snprintf(buf, cap,
            "MCDC-RELOAD status ERR\r\n"
            "MCDC-RELOAD rc %d\r\n"
            "MCDC-RELOAD ns %u\r\n"
            "MCDC-RELOAD dicts_loaded %u\r\n"
            "MCDC-RELOAD dicts_new %u\r\n"
            "MCDC-RELOAD dicts_reused %u\r\n"
            "MCDC-MCDC-RELOAD dicts_failed %u\r\n"
            "END\r\n",
            st->rc, st->namespaces, st->dicts_loaded, st->dicts_new, st->dicts_reused, st->dicts_failed);
    }
    return n;
}

static int reload_status_json(char *buf, size_t cap, mcdc_reload_status_t *st) {
    int n;
    if (st->rc == 0) {
        n = snprintf(buf, cap,
            "{\r\n"
            "\"status\": \"OK\",\r\n"
            "\"ns\": %u,\r\n"
            "\"dicts_loaded\": %u,\r\n"
            "\"dicts_new\": %u,\r\n"
            "\"dicts_reused\": %u,\r\n"
            "\"dicts_failed\": %u\r\n"
            "}\r\n",
            st->namespaces, st->dicts_loaded, st->dicts_new, st->dicts_reused, st->dicts_failed);
    } else if (st->err[0]) {
        n = snprintf(buf, cap,
            "{\r\n"
            "\"status\": \"ERR\",\r\n"
            "\"rc\": %d,\r\n"
            "\"msg\": \"%s\",\r\n"
            "\"ns\": %u,\r\n"
            "\"dicts_loaded\": %u,\r\n"
            "\"dicts_new\": %u,\r\n"
            "\"dicts_reused\": %u,\r\n"
            "\"dicts_failed\": %u\r\n"
            "}\r\n",
            st->rc, st->err, st->namespaces, st->dicts_loaded, st->dicts_new, st->dicts_reused, st->dicts_failed);
    } else {
        n = snprintf(buf, cap,
            "{\r\n"
            "\"status\": \"ERR\",\r\n"
            "\"rc\": %d,\r\n"
            "\"ns\": %u,\r\n"
            "\"dicts_loaded\": %u,\r\n"
            "\"dicts_new\": %u,\r\n"
            "\"dicts_reused\": %u,\r\n"
            "\"dicts_failed\": %u\r\n"
            "}\r\n",
            st->rc, st->namespaces, st->dicts_loaded, st->dicts_new, st->dicts_reused, st->dicts_failed);
    }
    return n;
}

static int build_reload_status(char **outp, size_t *lenp, int json) {

    mcdc_reload_status_t *st = mcdc_reload_dictionaries();
    if (!st) {
        return -1;
    }
    size_t cap = 512;
    char *buf = (char*)malloc(cap);
    int n = json?reload_status_json(buf, cap, st):reload_status_ascii(buf, cap, st);
    if (n < 0) { free(st);free(buf);return -1; }

    if ((size_t)n >= cap) {
        cap = (size_t)n + 1;
        char *nb = (char*)realloc(buf, cap);
        if (!nb) { free(buf); return -1; }
        buf = nb;
        n = json?reload_status_json(buf, cap, st):reload_status_ascii(buf, cap, st);
    }
    if (n < 0) { free(buf); return -1; }
    *outp = buf;
    *lenp = (size_t)n;
    free(st);
    return 0;
}

/* ----------  mcdc config ... ------------*/
static int cfg_ascii(char *buf, size_t cap, mcdc_cfg_t *c) {
    if (!c) return -1;

    const char *dict_dir = c->dict_dir ? c->dict_dir : "";
    const char *spool_dir= c->spool_dir ? c->spool_dir : "";

    int n = snprintf(buf, cap,
            "MCDC-CFG enable_comp %s\r\n"
            "MCDC-CFG enable_dict %s\r\n"
            "MCDC-CFG dict_dir %s\r\n"
            "MCDC-CFG dict_size %zu \r\n"
            "MCDC-CFG zstd_level %d \r\n"
            "MCDC-CFG min_comp_size %zu \r\n"
            "MCDC-CFG max_comp_size %zu \r\n"
            "MCDC-CFG compress_keys %s \r\n"
            "MCDC-CFG enable_training %s \r\n"
            "MCDC-CFG retraining_interval_s %" PRId64 "\r\n"
            "MCDC-CFG min_training_size %zu \r\n"
            "MCDC-CFG ewma_alpha %.6f\r\n"
            "MCDC-CFG retrain_drop %.6f\r\n"
            "MCDC-CFG train_mode %s\r\n"
            "MCDC-CFG gc_cool_period %d\r\n"
            "MCDC-CFG gc_quarantine_period %d\r\n"
            "MCDC-CFG dict_retain_max %d\r\n"
            "MCDC-CFG enable_sampling %s\r\n"
            "MCDC-CFG sample_p %.6f\r\n"
            "MCDC-CFG sample_window_duration %d\r\n"
            "MCDC-CFG spool_dir %s\r\n"
            "MCDC-CFG spool_max_bytes %zu\r\n"
            "END\r\n",
            b2s(c->enable_comp),
            b2s(c->enable_dict),
            dict_dir,
            c->dict_size,
            c->zstd_level,
            c->min_comp_size,
            c->max_comp_size,
            b2s(c->compress_keys),
            b2s(c->enable_training),
            (int64_t)c->retraining_interval_s,
            c->min_training_size,
            c->ewma_alpha,
            c->retrain_drop,
            train_mode_str(c->train_mode),
            c->gc_cool_period,
            c->gc_quarantine_period,
            c->dict_retain_max,
            b2s(c->enable_sampling),
            c->sample_p,
            c->sample_window_duration,
            spool_dir,
            c->spool_max_bytes
    );
    return n;
}

static int cfg_json(char *buf, size_t cap, mcdc_cfg_t *c) {
    if (!c) return -1;

    /* crude escape: assume paths don’t contain embedded quotes/newlines;
     if they might, add a tiny JSON-escape helper. */
    const char *dict_dir = c->dict_dir ? c->dict_dir : "";
    const char *spool_dir= c->spool_dir ? c->spool_dir : "";

    int n = snprintf(buf, cap,
            "{\r\n"
            "\"enable_comp\": %s,\r\n"
            "\"enable_dict\": %s,\r\n"
            "\"dict_dir\": \"%s\",\r\n"
            "\"dict_size\": %zu,\r\n"
            "\"zstd_level\": %d,\r\n"
            "\"min_comp_size\": %zu,\r\n"
            "\"max_comp_size\": %zu,\r\n"
            "\"compress_keys\": %s,\r\n"
            "\"enable_training\": %s,\r\n"
            "\"retraining_interval_s\": %" PRId64 ",\r\n"
            "\"min_training_size\": %zu,\r\n"
            "\"ewma_alpha\": %.6f,\r\n"
            "\"retrain_drop\": %.6f,\r\n"
            "\"train_mode\": \"%s\",\r\n"
            "\"gc_cool_period\": %d,\r\n"
            "\"gc_quarantine_period\": %d,\r\n"
            "\"dict_retain_max\": %d,\r\n"
            "\"enable_sampling\": %s,\r\n"
            "\"sample_p\": %.6f,\r\n"
            "\"sample_window_duration\": %d,\r\n"
            "\"spool_dir\": \"%s\",\r\n"
            "\"spool_max_bytes\": %zu\r\n"
            "}\r\n",
            b2s(c->enable_comp),
            b2s(c->enable_dict),
            dict_dir,
            c->dict_size,
            c->zstd_level,
            c->min_comp_size,
            c->max_comp_size,
            b2s(c->compress_keys),
            b2s(c->enable_training),
            (int64_t)c->retraining_interval_s,
            c->min_training_size,
            c->ewma_alpha,
            c->retrain_drop,
            train_mode_str(c->train_mode),
            c->gc_cool_period,
            c->gc_quarantine_period,
            c->dict_retain_max,
            b2s(c->enable_sampling),
            c->sample_p,
            c->sample_window_duration,
            spool_dir,
            c->spool_max_bytes
    );
    return n;
}

static int build_cfg(char **outp, size_t *lenp, int json) {
    mcdc_cfg_t *c = mcdc_config_get();
    if (!c) return -1;

    size_t cap = 2048;
    char *buf = (char*)malloc(cap);
    int n = json?cfg_json(buf, cap, c):cfg_ascii(buf, cap, c);
    if (n < 0) { free(buf);return -1; }

    if ((size_t)n >= cap) {
        cap = (size_t)n + 1;
        char *nb = (char*)realloc(buf, cap);
        if (!nb) { free(buf); return -1; }
        buf = nb;
        n = json?cfg_json(buf, cap, c):cfg_ascii(buf, cap, c);
    }
    if (n < 0) { free(buf); return -1; }
    *outp = buf;
    *lenp = (size_t)n;
    return 0;
}


/*    mcdc stats ... */

static int build_stats_ascii(char **outp, size_t *lenp,
                                const char *ns, const mcdc_stats_snapshot_t *s)
{
    /* Reserve a reasonable buffer; grow if needed. */
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) { *outp = NULL; *lenp = 0; return -ENOMEM; }

    int n = snprintf(buf, cap,
        "MCDC-STAT ns %s\r\n"
        "MCDC-STAT ewma_m %.6f\r\n"
        "MCDC-STAT baseline %.6f\r\n"
        "MCDC-STAT comp_ratio %.6f\r\n"
        "MCDC-STAT bytes_raw_total %" PRIu64 "\r\n"
        "MCDC-STAT bytes_cmp_total %" PRIu64 "\r\n"
        "MCDC-STAT reads_total %" PRIu64 "\r\n"
        "MCDC-STAT writes_total %" PRIu64 "\r\n"
        "MCDC-STAT dict_id %" PRIu32 "\r\n"
        "MCDC-STAT dict_size %" PRIu32 "\r\n"
        "MCDC-STAT total_dicts %" PRIu32 "\r\n"
        "MCDC-STAT train_mode %" PRIu32 "\r\n"
        "MCDC-STAT retrain %" PRIu32 "\r\n"
        "MCDC-STAT last_retrain_ms %" PRIu64 "\r\n"
        "MCDC-STAT trainer_runs %" PRIu64 "\r\n"
        "MCDC-STAT trainer_errs %" PRIu64 "\r\n"
        "MCDC-STAT trainer_ms_last %" PRIu64 "\r\n"
        "MCDC-STAT reservoir_bytes %" PRIu64 "\r\n"
        "MCDC-STAT reservoir_items %" PRIu64 "\r\n"
        "MCDC-STAT shadow_pct %" PRIu32 "\r\n"
        "MCDC-STAT shadow_samples %" PRIu64 "\r\n"
        "MCDC-STAT shadow_raw=%" PRIu64 "\r\n"
        "MCDC-STAT shadow_saved %" PRId64 "\r\n"
        "MCDC-STAT promotions %" PRIu32 "\r\n"
        "MCDC-STAT rollbacks %" PRIu32 "\r\n"
        "MCDC-STAT triggers_rise %" PRIu32 "\r\n"
        "MCDC-STAT triggers_drop %" PRIu32 "\r\n"
        "MCDC-STAT cooldown_left %" PRIu32 "\r\n"
        "MCDC-STAT compress_errs %" PRIu64 "\r\n"
        "MCDC-STAT decompress_errs %" PRIu64 "\r\n"
        "MCDC-STAT dict_miss_errs %" PRIu64 "\r\n"
        "MCDC-STAT skipped_min %" PRIu64 "\r\n"
        "MCDC-STAT skipped_max %" PRIu64 "\r\n"
        "MCDC-STAT skipped_incomp %" PRIu64 "\r\n"
        "END\r\n",
        ns ? ns : "global",
        s->ewma_m, s->baseline, s->cr_current,
        s->bytes_raw_total, s->bytes_cmp_total, s->reads_total, s->writes_total,
        s->dict_id, s->dict_size, s->total_dicts,
        s->train_mode, s->retrain_count, s->last_retrain_ms,
        s->trainer_runs, s->trainer_errs, s->trainer_ms_last,
        s->reservoir_bytes, s->reservoir_items,
        s->shadow_pct, s->shadow_samples, s->shadow_raw_total, s->shadow_saved_bytes,
        s->promotions, s->rollbacks,
        s->triggers_rise, s->triggers_drop, s->cooldown_win_left,
        s->compress_errs, s->decompress_errs, s->dict_miss_errs,
        s->skipped_comp_min_size, s->skipped_comp_max_size, s->skipped_comp_incomp
    );

    if (n < 0) { free(buf); *outp = NULL; *lenp = 0; return -ENOMEM;}
    *outp = buf;
    *lenp = (size_t)n;
    return 0;
}

static int build_stats_json(char **outp, size_t *lenp,
                               const char *ns, const mcdc_stats_snapshot_t *s)
{
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) { *outp = NULL; *lenp = 0; return -ENOMEM; }

    int n = snprintf(buf, cap,
        "{\r\n"
        "\"namespace\": \"%s\"," "\r\n"
        "\"ewma_m\": %.6f," "\r\n"
        "\"baseline\": %.6f," "\r\n"
        "\"comp_ratio\": %.6f," "\r\n"
        "\"bytes_raw_total\": %" PRIu64 "," "\r\n"
        "\"bytes_cmp_total\": %" PRIu64 "," "\r\n"
        "\"reads_total\": %" PRIu64 "," "\r\n"
        "\"writes_total\": %" PRIu64 "," "\r\n"
        "\"dict_id\": %" PRIu32 "," "\r\n"
        "\"dict_size\": %" PRIu32 "," "\r\n"
        "\"total_dicts\": %" PRIu32 "," "\r\n"
        "\"train_mode\": %" PRIu32 "," "\r\n"
        "\"retrain\": %" PRIu32 "," "\r\n"
        "\"last_retrain_ms\": %" PRIu64 "," "\r\n"
        "\"trainer_runs\": %" PRIu64 "," "\r\n"
        "\"trainer_errs\": %" PRIu64 "," "\r\n"
        "\"trainer_ms_last\": %" PRIu64 "," "\r\n"
        "\"reservoir_bytes\": %" PRIu64 "," "\r\n"
        "\"reservoir_items\": %" PRIu64 "," "\r\n"
        "\"shadow_pct\": %" PRIu32 "," "\r\n"
        "\"shadow_samples\": %" PRIu64 "," "\r\n"
        "\"shadow_raw\": %" PRIu64 "," "\r\n"
        "\"shadow_saved\": %" PRId64 "," "\r\n"
        "\"promotions\": %" PRIu32 "," "\r\n"
        "\"rollbacks\": %" PRIu32 "," "\r\n"
        "\"triggers_rise\": %" PRIu32 "," "\r\n"
        "\"triggers_drop\": %" PRIu32 "," "\r\n"
        "\"cooldown_left\": %" PRIu32 "," "\r\n"
        "\"compress_errs\": %" PRIu64 "," "\r\n"
        "\"decompress_errs\": %" PRIu64 "," "\r\n"
        "\"dict_miss_errs\": %" PRIu64 "," "\r\n"
        "\"skipped_min\": %" PRIu64 "," "\r\n"
        "\"skipped_max\": %" PRIu64 "," "\r\n"
        "\"skipped_incomp\": %" PRIu64 "\r\n"
        "}\r\n",
        ns ? ns : "global",
        s->ewma_m, s->baseline, s->cr_current,
        s->bytes_raw_total, s->bytes_cmp_total, s->reads_total, s->writes_total,
        s->dict_id, s->dict_size, s->total_dicts,
        s->train_mode, s->retrain_count, s->last_retrain_ms,
        s->trainer_runs, s->trainer_errs, s->trainer_ms_last,
        s->reservoir_bytes, s->reservoir_items,
        s->shadow_pct, s->shadow_samples, s->shadow_raw_total, s->shadow_saved_bytes,
        s->promotions, s->rollbacks,
        s->triggers_rise, s->triggers_drop, s->cooldown_win_left,
        s->compress_errs, s->decompress_errs, s->dict_miss_errs,
        s->skipped_comp_min_size, s->skipped_comp_max_size, s->skipped_comp_incomp
    );

    if (n < 0) { free(buf); *outp = NULL; *lenp = 0; return -ENOMEM; }

    *outp = buf;
    *lenp = (size_t)n;
    return 0;
}



/* Build ASCII multiline payload:
   NS global\r\n
   NS <ns>\r\n ...
   NS default\r\n (if not already present)
   END\r\n
*/
static int build_ns_ascii(char **outp, size_t *lenp) {
    size_t n = 0, i;
    const char **list = mcdc_list_namespaces(&n);   /* may return NULL or contain NULLs */

    /* First pass: compute size */
    size_t total = 0;
    total += sizeof("MCDC-NS global\r\n") - 1;  /* always include global */
    int has_default = 0;
    for (i = 0; i < n; i++) {
        const char *ns = list ? list[i] : NULL;
        if (!ns) continue;
        if (strcmp(ns, "default") == 0) has_default = 1;
        total += sizeof("MCDC-NS ") - 1 + strlen(ns) + sizeof("\r\n") - 1;
    }
    if (!has_default) total += sizeof("MCDC-NS default\r\n") - 1;
    total += sizeof("END\r\n") - 1;

    char *buf = (char *)malloc(total + 1);
    if (!buf) return -1;

    /* Second pass: fill */
    size_t off = 0;
    off += (size_t)sprintf(buf + off, "MCDC-NS global\r\n");
    for (i = 0; i < n; i++) {
        const char *ns = list ? list[i] : NULL;
        if (!ns) continue;
        off += (size_t)sprintf(buf + off, "MCDC-NS %s\r\n", ns);
    }
    if (!has_default) off += (size_t)sprintf(buf + off, "MCDC-NS default\r\n");
    off += (size_t)sprintf(buf + off, "END\r\n");
    buf[off] = '\0';

    if (list) free((void*)list);

    *outp = buf; *lenp = off;
    return 0;
}

/* Text protocol main entry point */

void process_mcdc_command_ascii(conn *c, token_t *tokens, const size_t ntokens)
{

    if (ntokens < 3 || strcmp(tokens[COMMAND_TOKEN].value, "mcdc") != 0) {
        out_string(c, "CLIENT_ERROR bad command");
        return;
    }

    const char *sub = tokens[COMMAND_TOKEN + 1].value;

    /* --- mcz sampler  --- */
    if (strcmp(sub, "sampler") == 0) {
        handle_mcdc_sampler_ascii(c, tokens, ntokens);
        return;
    }

    /* --- mcz config [json] --- */
    if (strcmp(sub, "config") == 0) {
        int want_json = 0;
        if (ntokens == 4) {
            const char *arg = tokens[COMMAND_TOKEN + 2].value;
            if (strcmp(arg, "json") == 0){
                want_json = 1;
            } else {
                out_string(c, "CLIENT_ERROR bad command");
                return;
            }
        } else if (ntokens > 4){
            out_string(c, "CLIENT_ERROR bad command");
            return;
        }

        char *payload = NULL; size_t plen = 0;
        int rc = build_cfg(&payload, &plen, want_json);
        if (rc != 0 || !payload) {
            if (rc != -ENOMEM){
                out_string(c, "SERVER_ERROR config serialization failed");
            } else {
                out_string(c, "SERVER_ERROR memory allocation failed");
            }
            if(payload) free(payload);
            return;
        }
        /* write_and_free takes ownership */
        write_and_free(c, payload, (int)plen);
        return;
    }

    /* mcz ns */
    if (strcmp(sub, "ns") == 0) {
        if (ntokens > 3){
            out_string(c, "CLIENT_ERROR bad command");
            return;
        }
        char *payload = NULL; size_t plen = 0;
        if (build_ns_ascii(&payload, &plen) != 0) {
            out_string(c, "SERVER_ERROR out of memory");
            if(payload) free(payload);
            return;
        }
        write_and_free(c, payload, plen);
        return;
    }
    /*  mcz reload [json] */
    if(strcmp(sub, "reload") == 0) {
        int want_json = 0;
        if (ntokens == 4 && tokens[COMMAND_TOKEN + 2].value &&
            strcmp(tokens[COMMAND_TOKEN + 2].value, "json") == 0) {
            want_json = 1;
        } else if (ntokens >= 4){
            out_string(c, "CLIENT_ERROR bad command");
            return;
        }
        char *payload = NULL; size_t plen = 0;
        if (build_reload_status(&payload, &plen, want_json) != 0) {
            out_string(c, "SERVER_ERROR out of memory");
            if(payload) free(payload);
            return;
        }
        write_and_free(c, payload, plen);
        return;
    }

    /* mcz stats */
    if (ntokens < 3 || strcmp(tokens[COMMAND_TOKEN + 1].value, "stats") != 0) {
        out_string(c, "CLIENT_ERROR bad command");
        return;
    }

    const char *ns = NULL; /* NULL => global */
    int want_json = 0;

    if (ntokens >= 4) {
        const char *arg1 = tokens[COMMAND_TOKEN + 2].value;
        if (strcmp(arg1, "global") == 0) {
            ns = NULL;
        } else {
            ns = arg1; /* includes "default" or any other namespace */
        }
    }
    if (ntokens == 5) {
        const char *arg2 = tokens[COMMAND_TOKEN + 3].value;
        if (strcmp(arg2, "json") == 0) {
            want_json = 1;
        } else {
            out_string(c, "CLIENT_ERROR bad command");
            return;
        }
    } else if (ntokens > 5){
        out_string(c, "CLIENT_ERROR bad command");
        return;
    }

    /* Build snapshot */
    mcdc_stats_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    size_t nlen = ns ? strlen(ns) : 0;
    int rc = mcdc_get_stats_snapshot(&snap, ns, nlen);
    if (rc < 0) {
        if (rc != -ENOENT){
            out_string(c, "SERVER_ERROR mcdc_get_stats_snapshot failed");
        } else {
            out_string(c, "CLIENT_ERROR namespace does not exist");
        }
        return;
    }

    /* Serialize */
    char *out = NULL; size_t len = 0;

    if (want_json) {
       rc = build_stats_json(&out, &len, ns ? ns : "global", &snap);
    } else {
       rc = build_stats_ascii(&out, &len, ns ? ns : "global", &snap);
    }
    if (rc < 0) { out_string(c, "SERVER_ERROR memory allocation failed"); return; }
    if (!out) { out_string(c, "SERVER_ERROR serialization failed"); return; }

    write_and_free(c, out, len);
}
