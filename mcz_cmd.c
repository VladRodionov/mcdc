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
 * mcz_cmd.c - Implementation of MCZ command extensions for memcached.
 *
 * This file adds support for custom ASCII and binary protocol commands:
 *
 *   - "mcz stats [<namespace>|global|default] [json]"
 *       Dump statistics snapshots in text or JSON form.
 *
 *   - "mcz ns"
 *       List active namespaces, including "global" and "default".
 *
 *   - "mcz config [json]"
 *       Show current configuration, either as text lines or as a JSON object.
 *
 *   - "mcz sampler [start|stop|status]"
 *       Sampler control (spooling data, incoming key-value pairs to a file
 *           for further analysis and dictionary creation).
 *
 *   - PROTOCOL_BINARY_CMD_MCZ_STATS (0xE1)
 *       Binary version of "mcz stats".
 *
 *   - PROTOCOL_BINARY_CMD_MCZ_NS (0xE2)
 *       Binary namespace listing.
 *
 *   - PROTOCOL_BINARY_CMD_MCZ_CFG (0xE3)
 *       Binary configuration dump.
 *
 *   - PROTOCOL_BINARY_CMD_MCZ_SAMPLER (0xE4)
 *       Binary sampler.
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

#include "mcz_cmd.h"
#include "mcz_config.h"

#include "mcz_sampling.h"
#include "proto_bin.h"

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1


/* Map train mode to string */
static const char *train_mode_str(mcz_train_mode_t m) {
    switch (m) {
    case MCZ_TRAIN_FAST:     return "FAST";
    case MCZ_TRAIN_OPTIMIZE: return "OPTIMIZE";
    default:                 return "UNKNOWN";
    }
}

static inline const char *b2s(bool v) { return v ? "true" : "false"; }

/* Small helper: write a dynamic response safely */
static void write_buf(conn *c, const char *buf, size_t len) {
    /* memcached has helpers; write_and_free() takes ownership */
    char *out = malloc(len + 1);
    if (!out) { out_string(c, "SERVER_ERROR out of memory"); return; }
    memcpy(out, buf, len);
    out[len] = '\0';
    write_and_free(c, out, len);
}

/* ---------- status builders ---------- */
static int build_sampler_status_ascii(char **outp, size_t *lenp) {
    mcz_sampler_status_t st;
    mcz_sampler_get_status(&st);

    size_t cap = 2048;
    char *buf = (char*)malloc(cap);
    if (!buf) return -1;
    size_t off = 0;

#define APP(...) do { \
        int need = snprintf(NULL, 0, __VA_ARGS__); \
        if (need < 0) { free(buf); return -1; } \
        size_t ns = (size_t)need; \
        if (off + ns + 1 > cap) { \
            size_t ncap = (cap*2 > off + ns + 64) ? cap*2 : off + ns + 64; \
            char *nb = (char*)realloc(buf, ncap); \
            if (!nb) { free(buf); return -1; } \
            buf = nb; cap = ncap; \
        } \
        off += (size_t)sprintf(buf + off, __VA_ARGS__); \
    } while (0)

    APP("SAMPLER configured %s\r\n", st.configured ? "true" : "false");
    APP("SAMPLER running %s\r\n",    st.running    ? "true" : "false");
    APP("SAMPLER bytes_written %" PRIu64 "\r\n", (uint64_t)st.bytes_written);
    APP("SAMPLER bytes_collected %" PRIu64 "\r\n",   (uint64_t)st.bytes_collected);
    APP("SAMPLER path %s\r\n",       st.current_path[0] ? st.current_path : "");
    APP("END\r\n");
#undef APP

    *outp = buf; *lenp = off;
    return 0;
}

static int build_sampler_status_json(char **outp, size_t *lenp) {
    mcz_sampler_status_t st;
    mcz_sampler_get_status(&st);
    size_t cap = 2048;
    char *buf = (char*)malloc(cap);
    if (!buf) return -1;

    int n = snprintf(buf, cap,
        "{\r\n"
          "\"configured\":%s,\r\n"
          "\"running\":%s,\r\n"
          "\"bytes_written\":%" PRIu64 ",\r\n"
          "\"queue_collected\":%" PRIu64 ",\r\n"
          "\"path\":\"%s\"\r\n"
        "}\r\n",
        st.configured ? "true" : "false",
        st.running    ? "true" : "false",
        (uint64_t)st.bytes_written,
        (uint64_t)st.bytes_collected,
        st.current_path[0] ? st.current_path : ""
    );
    if (n < 0) { free(buf); return -1; }
    if ((size_t)n >= cap) {
        cap = (size_t)n + 1;
        char *nb = (char*)realloc(buf, cap);
        if (!nb) { free(buf); return -1; }
        buf = nb;
        n = snprintf(buf, cap,
            "{"
              "\"configured\":%s,"
              "\"running\":%s,"
              "\"bytes_written\":%" PRIu64 ","
              "\"bytes_collected\":%" PRIu64 ","
              "\"path\":\"%s\""
            "}\r\n",
            st.configured ? "true" : "false",
            st.running    ? "true" : "false",
            (uint64_t)st.bytes_written,
            (uint64_t)st.bytes_collected,
            st.current_path[0] ? st.current_path : ""
        );
        if (n < 0) { free(buf); return -1; }
    }
    *outp = buf; *lenp = (size_t)n;
    return 0;
}

/* ---------- ASCII: mcz sampler ... ---------- */
static void handle_mcz_sampler_ascii(conn *c, token_t *tokens, size_t ntokens) {
    if (ntokens < 3) { out_string(c, "CLIENT_ERROR usage: mcz sampler <start|stop|status> [json]"); return; }
    const char *verb = tokens[COMMAND_TOKEN + 2].value;

    if (strcmp(verb, "start") == 0) {
        int rc = mcz_sampler_start();
        if (rc == 0) {
            out_string(c, "OK");
        } else if (rc == 1) {
            out_string(c, "RUNNING");
        } else {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "SERVER_ERROR sampler_start rc=%d", rc);
            out_string(c, tmp);
        }
        return;
    } else if (strcmp(verb, "stop") == 0) {
        int rc = mcz_sampler_stop();
        if (rc == 0) {
            out_string(c, "OK");
        } else if (rc == 1) {
            out_string(c, "NOT RUNNING");
        } else {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "SERVER_ERROR sampler_stop rc=%d", rc);
            out_string(c, tmp);
        }
        return;
    } else if (strcmp(verb, "status") == 0) {
        int want_json = 0;
        if (ntokens > 3 && tokens[COMMAND_TOKEN + 3].value &&
            strcmp(tokens[COMMAND_TOKEN + 3].value, "json") == 0) {
            want_json = 1;
        }
        char *payload = NULL; size_t plen = 0;
        int rc = want_json ? build_sampler_status_json(&payload, &plen)
                           : build_sampler_status_ascii(&payload, &plen);
        if (rc != 0 || !payload) { out_string(c, "SERVER_ERROR sampler_status"); return; }
        write_buf(c, payload, plen);
        return;
    }

    out_string(c, "CLIENT_ERROR usage: mcz sampler <start|stop|status> [json]");
}

/* ---------- ASCII builder: "CFG key value" per line + END ---------- */
static int build_cfg_ascii(char **outp, size_t *lenp) {
    mcz_cfg_t *c = mcz_config_get();
    if (!c) return -1;

    /* Estimate generously to avoid multiple reallocs. */
    size_t cap = 2048;
    char *buf = (char *)malloc(cap);
    if (!buf) return -ENOMEM;
    size_t off = 0;

#define APP(...) do { \
        int need = snprintf(NULL, 0, __VA_ARGS__); \
        if (need < 0) { free(buf); return -1; } \
        size_t need_sz = (size_t)need; \
        if (off + need_sz + 1 > cap) { \
            size_t ncap = (cap * 2 > off + need_sz + 64) ? cap * 2 : off + need_sz + 64; \
            char *nb = (char *)realloc(buf, ncap); \
            if (!nb) { free(buf); return -1; } \
            buf = nb; cap = ncap; \
        } \
        off += (size_t)sprintf(buf + off, __VA_ARGS__); \
    } while (0)

    APP("CFG enable_comp %s\r\n", b2s(c->enable_comp));
    APP("CFG enable_dict %s\r\n", b2s(c->enable_dict));
    APP("CFG dict_dir %s\r\n", c->dict_dir ? c->dict_dir : "");
    APP("CFG dict_size %zu\r\n", c->dict_size);
    APP("CFG zstd_level %d\r\n", c->zstd_level);
    APP("CFG min_comp_size %zu\r\n", c->min_comp_size);
    APP("CFG max_comp_size %zu\r\n", c->max_comp_size);
    APP("CFG compress_keys %s\r\n", b2s(c->compress_keys));

    APP("CFG enable_training %s\r\n", b2s(c->enable_training));
    APP("CFG retraining_interval_s %" PRId64 "\r\n", (int64_t)c->retraining_interval_s);
    APP("CFG min_training_size %zu\r\n", c->min_training_size);
    APP("CFG ewma_alpha %.6f\r\n", c->ewma_alpha);
    APP("CFG retrain_drop %.6f\r\n", c->retrain_drop);
    APP("CFG train_mode %s\r\n", train_mode_str(c->train_mode));

    APP("CFG gc_cool_period %d\r\n", c->gc_cool_period);
    APP("CFG gc_quarantine_period %d\r\n", c->gc_quarantine_period);
    APP("CFG dict_retain_max %d\r\n", c->dict_retain_max);

    APP("CFG enable_sampling %s\r\n", b2s(c->enable_sampling));
    APP("CFG sample_p %.6f\r\n", c->sample_p);
    APP("CFG sample_window_duration %d\r\n", c->sample_window_duration);
    APP("CFG spool_dir %s\r\n", c->spool_dir ? c->spool_dir : "");
    APP("CFG spool_max_bytes %zu\r\n", c->spool_max_bytes);

    APP("END\r\n");

#undef APP

    *outp = buf; *lenp = off;
    return 0;
}

/* ---------- JSON builder: compact JSON ---------- */
/* (Binary uses this JSON as the value; ASCII can request it via 'mcz config json') */
static int build_cfg_json(char **outp, size_t *lenp) {
    mcz_cfg_t *c = mcz_config_get();
    if (!c) return -1;

    /* crude escape: assume paths don’t contain embedded quotes/newlines;
       if they might, add a tiny JSON-escape helper. */
    const char *dict_dir = c->dict_dir ? c->dict_dir : "";
    const char *spool_dir= c->spool_dir ? c->spool_dir : "";

    size_t cap = 2048;
    char *buf = (char *)malloc(cap);
    if (!buf) return -ENOMEM;

    int n = snprintf(buf, cap,
        "{\r\n"
        "\"enable_comp\":%s,\r\n"
        "\"enable_dict\":%s,\r\n"
        "\"dict_dir\":\"%s\",\r\n"
        "\"dict_size\":%zu,\r\n"
        "\"zstd_level\":%d,\r\n"
        "\"min_comp_size\":%zu,\r\n"
        "\"max_comp_size\":%zu,\r\n"
        "\"compress_keys\":%s,\r\n"

        "\"enable_training\":%s,\r\n"
        "\"retraining_interval_s\":%" PRId64 ",\r\n"
        "\"min_training_size\":%zu,\r\n"
        "\"ewma_alpha\":%.6f,\r\n"
        "\"retrain_drop\":%.6f,\r\n"
        "\"train_mode\":\"%s\",\r\n"

        "\"gc_cool_period\":%d,\r\n"
        "\"gc_quarantine_period\":%d,\r\n"
        "\"dict_retain_max\":%d,\r\n"

        "\"enable_sampling\":%s,\r\n"
        "\"sample_p\":%.6f,\r\n"
        "\"sample_window_duration\":%d,\r\n"
        "\"spool_dir\":\"%s\",\r\n"
        "\"spool_max_bytes\":%zu\r\n"
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

    *outp = buf; *lenp = (size_t)n;
    return 0;
}

static int build_stats_ascii(char **outp, size_t *lenp,
                                const char *ns, const mcz_stats_snapshot_t *s)
{
    /* Reserve a reasonable buffer; grow if needed. */
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) { *outp = NULL; *lenp = 0; return -ENOMEM; }

    int n = snprintf(buf, cap,
        "STAT ns=%s\r\n"
        "STAT ewma_m=%.6f\r\n"
        "STAT baseline=%.6f\r\n"
        "STAT comp_ratio=%.6f\r\n"
        "STAT bytes_raw_total=%" PRIu64 "\r\n"
        "STAT bytes_cmp_total=%" PRIu64 "\r\n"
        "STAT reads_total=%" PRIu64 "\r\n"
        "STAT writes_total=%" PRIu64 "\r\n"
        "STAT dict_id=%" PRIu32 "\r\n"
        "STAT dict_size=%" PRIu32 "\r\n"
        "STAT total_dicts=%" PRIu32 "\r\n"
        "STAT train_mode=%" PRIu32 "\r\n"
        "STAT retrain=%" PRIu32 "\r\n"
        "STAT last_retrain_ms=%" PRIu64 "\r\n"
        "STAT trainer_runs=%" PRIu64 "\r\n"
        "STAT trainer_errs=%" PRIu64 "\r\n"
        "STAT trainer_ms_last=%" PRIu64 "\r\n"
        "STAT reservoir_bytes=%" PRIu64 "\r\n"
        "STAT reservoir_items=%" PRIu64 "\r\n"
        "STAT shadow_pct=%" PRIu32 "\r\n"
        "STAT shadow_samples=%" PRIu64 "\r\n"
        "STAT shadow_raw=%" PRIu64 "\r\n"
        "STAT shadow_saved=%" PRId64 "\r\n"
        "STAT promotions=%" PRIu32 "\r\n"
        "STAT rollbacks=%" PRIu32 "\r\n"
        "STAT triggers_rise=%" PRIu32 "\r\n"
        "STAT triggers_drop=%" PRIu32 "\r\n"
        "STAT cooldown_left=%" PRIu32 "\r\n"
        "STAT compress_errs=%" PRIu64 "\r\n"
        "STAT decompress_errs=%" PRIu64 "\r\n"
        "STAT dict_miss_errs=%" PRIu64 "\r\n"
        "STAT skipped_min=%" PRIu64 "\r\n"
        "STAT skipped_max=%" PRIu64 "\r\n"
        "STAT skipped_incomp=%" PRIu64 "\r\n"
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
    if ((size_t)n >= cap) {
        /* grow and retry once */
        cap = (size_t)n + 1;
        char *b2 = malloc(cap);
        if (!b2) { free(buf); *outp = NULL; *lenp = 0; return -ENOMEM; }
        n = snprintf(b2, cap, "%s", buf); /* or rebuild the whole string (better) */
        free(buf);
        buf = b2;
    }

    *outp = buf;
    *lenp = (size_t)n;
    return 0;
}

static int build_stats_json(char **outp, size_t *lenp,
                               const char *ns, const mcz_stats_snapshot_t *s)
{
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) { *outp = NULL; *lenp = 0; return -ENOMEM; }

    int n = snprintf(buf, cap,
        "{" "\r\n"
        "\"namespace\":\"%s\"," "\r\n"
        "\"ewma_m\":%.6f," "\r\n"
        "\"baseline\":%.6f," "\r\n"
        "\"comp_ratio\":%.6f," "\r\n"
        "\"bytes_raw_total\":%" PRIu64 "," "\r\n"
        "\"bytes_cmp_total\":%" PRIu64 "," "\r\n"
        "\"reads_total\":%" PRIu64 "," "\r\n"
        "\"writes_total\":%" PRIu64 "," "\r\n"
        "\"dict_id\":%" PRIu32 "," "\r\n"
        "\"dict_size\":%" PRIu32 "," "\r\n"
        "\"total_dicts\":%" PRIu32 "," "\r\n"
        "\"train_mode\":%" PRIu32 "," "\r\n"
        "\"retrain\":%" PRIu32 "," "\r\n"
        "\"last_retrain_ms\":%" PRIu64 "," "\r\n"
        "\"trainer_runs\":%" PRIu64 "," "\r\n"
        "\"trainer_errs\":%" PRIu64 "," "\r\n"
        "\"trainer_ms_last\":%" PRIu64 "," "\r\n"
        "\"reservoir_bytes\":%" PRIu64 "," "\r\n"
        "\"reservoir_items\":%" PRIu64 "," "\r\n"
        "\"shadow_pct\":%" PRIu32 "," "\r\n"
        "\"shadow_samples\":%" PRIu64 "," "\r\n"
        "\"shadow_raw\":%" PRIu64 "," "\r\n"
        "\"shadow_saved\":%" PRId64 "," "\r\n"
        "\"promotions\":%" PRIu32 "," "\r\n"
        "\"rollbacks\":%" PRIu32 "," "\r\n"
        "\"triggers_rise\":%" PRIu32 "," "\r\n"
        "\"triggers_drop\":%" PRIu32 "," "\r\n"
        "\"cooldown_left\":%" PRIu32 "," "\r\n"
        "\"compress_errs\":%" PRIu64 "," "\r\n"
        "\"decompress_errs\":%" PRIu64 "," "\r\n"
        "\"dict_miss_errs\":%" PRIu64 "," "\r\n"
        "\"skipped_min\":%" PRIu64 "," "\r\n"
        "\"skipped_max\":%" PRIu64 "," "\r\n"
        "\"skipped_incomp\":%" PRIu64 "\r\n"
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
    if ((size_t)n >= cap) {
        cap = (size_t)n + 1;
        char *b2 = malloc(cap);
        if (!b2) { free(buf); *outp = NULL; *lenp = 0; return -ENOMEM; }
        n = snprintf(b2, cap, "%s", buf); /* or rebuild exactly */
        free(buf);
        buf = b2;
    }

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
    const char **list = mcz_list_namespaces(&n);   /* may return NULL or contain NULLs */

    /* First pass: compute size */
    size_t total = 0;
    total += sizeof("NS global\r\n") - 1;  /* always include global */
    int has_default = 0;
    for (i = 0; i < n; i++) {
        const char *ns = list ? list[i] : NULL;
        if (!ns) continue;
        if (strcmp(ns, "default") == 0) has_default = 1;
        total += sizeof("NS ") - 1 + strlen(ns) + sizeof("\r\n") - 1;
    }
    if (!has_default) total += sizeof("NS default\r\n") - 1;
    total += sizeof("END\r\n") - 1;

    char *buf = (char *)malloc(total + 1);
    if (!buf) return -1;

    /* Second pass: fill */
    size_t off = 0;
    off += (size_t)sprintf(buf + off, "NS global\r\n");
    for (i = 0; i < n; i++) {
        const char *ns = list ? list[i] : NULL;
        if (!ns) continue;
        off += (size_t)sprintf(buf + off, "NS %s\r\n", ns);
    }
    if (!has_default) off += (size_t)sprintf(buf + off, "NS default\r\n");
    off += (size_t)sprintf(buf + off, "END\r\n");
    buf[off] = '\0';

    /* If mcz_list_namespaces() returns heap, free it; otherwise remove this free. */
    if (list) free((void*)list);

    *outp = buf; *lenp = off;
    return 0;
}

void process_mcz_command_ascii(conn *c, token_t *tokens, const size_t ntokens)
{
    
    if (ntokens < 2 || strcmp(tokens[COMMAND_TOKEN].value, "mcz") != 0) {
        out_string(c, "CLIENT_ERROR bad command");
        return;
    }
    if (ntokens < 3) {
        out_string(c, "CLIENT_ERROR usage: mcz <stats|ns|config|sampler> ...");
        return;
    }

    const char *sub = tokens[COMMAND_TOKEN + 1].value;

    if (strcmp(sub, "sampler") == 0) {
        handle_mcz_sampler_ascii(c, tokens, ntokens);
        return;
    }

    /* --- mcz config [json] --- */
    if (strcmp(sub, "config") == 0) {
        int want_json = 0;
        if (ntokens >= 4) {
            const char *arg = tokens[COMMAND_TOKEN + 2].value;
            if (strcmp(arg, "json") == 0){
                want_json = 1;
            } else {
                out_string(c, "CLIENT_ERROR bad command");
                return;
            }
        }

        char *payload = NULL; size_t plen = 0;
        int rc = want_json ? build_cfg_json(&payload, &plen)
                           : build_cfg_ascii(&payload, &plen);
        if (rc != 0 || !payload) {
            if (rc != -ENOMEM){
                out_string(c, "SERVER_ERROR config serialization failed");
            } else {
                out_string(c, "SERVER_ERROR memory allocation failed");
            }
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
            return;
        }
        write_buf(c, payload, plen);
        free(payload);
        return;
    }
    /* mcz stats */
    if (ntokens < 3 || strcmp(tokens[COMMAND_TOKEN + 1].value, "stats") != 0) {
        out_string(c, "CLIENT_ERROR usage: mcz stats [namespace|global|default] [json]");
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
    if (ntokens >= 5) {
        const char *arg2 = tokens[COMMAND_TOKEN + 3].value;
        if (strcmp(arg2, "json") == 0) want_json = 1;
    }

    /* Build snapshot */
    mcz_stats_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    size_t nlen = ns ? strlen(ns) : 0;
    int rc = mcz_get_stats_snapshot(&snap, ns, nlen);
    if (rc < 0) {
        if (rc != -ENOENT){
            out_string(c, "SERVER_ERROR mcz_get_stats_snapshot failed");
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

    write_buf(c, out, len);
    free(out);
}

void process_mcz_sampler_bin(conn *c)
{
    const protocol_binary_request_header *req = &c->binary_header;

    uint8_t  extlen  = req->request.extlen;
    uint16_t keylen  = req->request.keylen;
    uint32_t bodylen = req->request.bodylen;

    if (extlen != 0 || keylen == 0 || bodylen != keylen) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
        return;
    }

    /* Key is the action */
    size_t alen = (size_t)keylen;
    /* Body in rbuf immediately follows the 24-byte header */
    const char *body = (const char *)c->rbuf + sizeof(protocol_binary_request_header);
    const char *action    = body + extlen;      /* start of key */
    /* Normalize action into a C string (stack buffer) */
    char act[16];
    size_t n = (alen < sizeof(act)-1) ? alen : (sizeof(act)-1);
    memcpy(act, action, n);
    act[n] = '\0';

    protocol_binary_response_header h;
    memset(&h, 0, sizeof(h));
    h.response.magic    = PROTOCOL_BINARY_RES;
    h.response.opcode   = PROTOCOL_BINARY_CMD_MCZ_SAMPLER;
    h.response.keylen   = 0;
    h.response.extlen   = 0;
    h.response.datatype = PROTOCOL_BINARY_RAW_BYTES;
    h.response.status   = PROTOCOL_BINARY_RESPONSE_SUCCESS;
    h.response.opaque   = req->request.opaque;
    h.response.cas      = 0;

    if (strcmp(act, "start") == 0) {
        int rc = mcz_sampler_start();
        const char *msg = (rc == 0) ? "OK\r\n" :
                          (rc == 1) ? "RUNNING\r\n" :
                                      "ERROR\r\n";
        size_t mlen = strlen(msg);

        h.response.bodylen = (uint32_t)mlen;
        size_t total = sizeof(h) + mlen;
        char *resp = malloc(total);
        if (!resp) { write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, 0); return; }
        memcpy(resp, &h, sizeof(h));
        memcpy(resp + sizeof(h), msg, mlen);
        write_and_free(c, resp, (int)total);
        return;
    }
    else if (strcmp(act, "stop") == 0) {
        int rc = mcz_sampler_stop();
        const char *msg = (rc == 0) ? "OK\r\n" :
                          (rc == 1) ? "NOT RUNNING\r\n" :
                                      "ERROR\r\n";
        size_t mlen = strlen(msg);

        h.response.bodylen = (uint32_t)mlen;
        size_t total = sizeof(h) + mlen;
        char *resp = malloc(total);
        if (!resp) { write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, 0); return; }
        memcpy(resp, &h, sizeof(h));
        memcpy(resp + sizeof(h), msg, mlen);
        write_and_free(c, resp, (int)total);
        return;
    } else if (strcmp(act, "status") == 0) {
        char *payload = NULL; size_t plen = 0;
        if (build_sampler_status_json(&payload, &plen) != 0 || !payload) {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
            return;
        }
        h.response.bodylen = (uint32_t)plen;
        size_t total = sizeof(h) + plen;
        char *resp = (char*)malloc(total);
        if (!resp) { free(payload); write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, 0); return; }
        memcpy(resp, &h, sizeof(h));
        memcpy(resp + sizeof(h), payload, plen);
        free(payload);
        write_and_free(c, resp, (int)total);
        return;
    }

    write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
}

void process_mcz_cfg_bin(conn *c)
{
    const protocol_binary_request_header *req = &c->binary_header;

    /* In 1.6.38, these are host-order already */
    uint8_t  extlen  = req->request.extlen;
    uint16_t keylen  = req->request.keylen;
    uint32_t bodylen = req->request.bodylen;

    /* Expect no body for this op */
    if (extlen != 0 || keylen != 0 || bodylen != 0) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
        return;
    }

    /* Build JSON payload */
    char *payload = NULL; size_t plen = 0;
    if (build_cfg_json(&payload, &plen) != 0 || !payload) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
        return;
    }

    /* Compose response header + value into one contiguous buffer */
    size_t total = sizeof(protocol_binary_response_header) + plen;
    char *resp = (char *)malloc(total);
    if (!resp) { free(payload); write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, 0); return; }

    protocol_binary_response_header h;
    memset(&h, 0, sizeof(h));
    h.response.magic    = PROTOCOL_BINARY_RES;
    h.response.opcode   = PROTOCOL_BINARY_CMD_MCZ_CFG;   /* 0xE3 */
    h.response.keylen   = 0;
    h.response.extlen   = 0;
    h.response.datatype = PROTOCOL_BINARY_RAW_BYTES;
    h.response.status   = PROTOCOL_BINARY_RESPONSE_SUCCESS;
    h.response.bodylen  = (uint32_t)plen;                /* host-order; core swaps on send */
    h.response.opaque   = req->request.opaque;
    h.response.cas      = 0;

    memcpy(resp, &h, sizeof(h));
    if (plen) memcpy(resp + sizeof(h), payload, plen);
    free(payload);

    write_and_free(c, resp, (int)total);
}

void process_mcz_stats_bin(conn *c)
{
    const protocol_binary_request_header *req = &c->binary_header;
    uint8_t  extlen  = req->request.extlen;
    uint16_t keylen  = req->request.keylen;
    uint32_t bodylen = req->request.bodylen;

    /* Basic shape validation */
    if (bodylen < (uint32_t)extlen + (uint32_t)keylen) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
        return;
    }

    /* Body layout: [extras=extlen][key=keylen][value=remaining] — we expect no extras/value */
    const char *ns   = NULL;
    size_t      nslen= 0;

    if (keylen) {
        /* Body in rbuf immediately follows the 24-byte header */
        const char *body = (const char *)c->rbuf + sizeof(protocol_binary_request_header);
        ns    = body + extlen;      /* start of key */
        nslen = (size_t)keylen;     /* not NUL-terminated */
        if ((nslen == sizeof("global") - 1) && strncmp ("global", ns, nslen) == 0){
            ns = NULL;
            nslen = 0;
        }
    }

    /* Build snapshot (ns==NULL -> global) */
    mcz_stats_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    if (mcz_get_stats_snapshot(&snap, ns, nslen) < 0) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
        return;
    }

    /* JSON payload */
    char *payload = NULL;
    size_t plen = 0;
    
    if (build_stats_json(&payload, &plen, ns ? ns : "global", &snap) < 0) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, 0);
        return;
    }

    /* Compose full binary response (header + value) into one contiguous buffer */
    size_t total = sizeof(protocol_binary_response_header) + plen;
    char *resp = (char *)malloc(total);
    if (!resp) {
        free(payload);
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, 0);
        return;
    }

    protocol_binary_response_header h;
    memset(&h, 0, sizeof(h));
    h.response.magic    = PROTOCOL_BINARY_RES;
    h.response.opcode   = PROTOCOL_BINARY_CMD_MCZ_STATS;      /* 0xE1 */
    h.response.keylen   = htons(0);
    h.response.extlen   = 0;
    h.response.datatype = PROTOCOL_BINARY_RAW_BYTES;
    h.response.status   = htons(PROTOCOL_BINARY_RESPONSE_SUCCESS);
    h.response.bodylen  = htonl((uint32_t)plen);
    h.response.opaque   = req->request.opaque;
    h.response.cas      = 0;

    memcpy(resp, &h, sizeof(h));
    if (plen) memcpy(resp + sizeof(h), payload, plen);
    free(payload);

    write_and_free(c, resp, (int)total);
}

/* mcz_cmd.c continued */

static int build_ns_text_value(char **outp, size_t *lenp) {
    size_t n = 0, i;
    const char **list = mcz_list_namespaces(&n);

    /* First pass: compute size of newline-separated blob */
    size_t total = 0;
    total += sizeof("global\n") - 1;
    int has_default = 0;
    for (i = 0; i < n; i++) {
        const char *ns = list ? list[i] : NULL;
        if (!ns) continue;
        if (strcmp(ns, "default") == 0) has_default = 1;
        total += strlen(ns) + 1; /* '\n' */
    }
    if (!has_default) total += sizeof("default\n") - 1;

    char *buf = (char *)malloc(total + 1);
    if (!buf) return -1;

    size_t off = 0;
    memcpy(buf + off, "global\n", 7); off += 7;
    for (i = 0; i < n; i++) {
        const char *ns = list ? list[i] : NULL;
        if (!ns) continue;
        size_t L = strlen(ns);
        memcpy(buf + off, ns, L); off += L;
        buf[off++] = '\n';
    }
    if (!has_default) {
        memcpy(buf + off, "default\n", 8); off += 8;
    }
    buf[off] = '\0';

    if (list) free((void*)list);

    *outp = buf; *lenp = off;
    return 0;
}

void process_mcz_ns_bin(conn *c)
{
    const protocol_binary_request_header *req = &c->binary_header;

    /* In 1.6.38, these are already host-order */
    uint8_t  extlen  = req->request.extlen;
    uint16_t keylen  = req->request.keylen;
    uint32_t bodylen = req->request.bodylen;

    /* Expect no extras/key/value for this op */
    if (extlen != 0 || keylen != 0 || bodylen != 0) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
        return;
    }

    char *payload = NULL; size_t plen = 0;
    if (build_ns_text_value(&payload, &plen) != 0) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, 0);
        return;
    }

    size_t total = sizeof(protocol_binary_response_header) + plen;
    char *resp = (char *)malloc(total);
    if (!resp) { free(payload); write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, 0); return; }

    protocol_binary_response_header h;
    memset(&h, 0, sizeof(h));
    h.response.magic    = PROTOCOL_BINARY_RES;
    h.response.opcode   = PROTOCOL_BINARY_CMD_MCZ_NS;
    h.response.keylen   = 0;
    h.response.extlen   = 0;
    h.response.datatype = PROTOCOL_BINARY_RAW_BYTES;
    h.response.status   = PROTOCOL_BINARY_RESPONSE_SUCCESS;
    h.response.bodylen  = (uint32_t)plen;       /* host-order; core will swap on write */
    h.response.opaque   = req->request.opaque;
    h.response.cas      = 0;

    memcpy(resp, &h, sizeof(h));
    if (plen) memcpy(resp + sizeof(h), payload, plen);
    free(payload);

    write_and_free(c, resp, (int)total);
}
