/* mcz_cmd.c */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>

#include <stdlib.h>
#include <arpa/inet.h>

#include "mcz_cmd.h"
#include "proto_bin.h"

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1

/* Small helper: write a dynamic response safely */
static void write_buf(conn *c, const char *buf, size_t len) {
    /* memcached has helpers; write_and_free() takes ownership */
    char *out = malloc(len + 1);
    if (!out) { out_string(c, "SERVER_ERROR out of memory"); return; }
    memcpy(out, buf, len);
    out[len] = '\0';
    write_and_free(c, out, len);
}

static int dump_ascii_into_buf(char **outp, size_t *lenp,
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

static int dump_json_into_buf(char **outp, size_t *lenp,
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
    
    /* ASCII: mcz <subcmd> ... */
    if (ntokens < 2 || strcmp(tokens[COMMAND_TOKEN].value, "mcz") != 0) {
        out_string(c, "CLIENT_ERROR bad command");
        return;
    }

    const char *sub = (ntokens > 2) ? tokens[COMMAND_TOKEN + 1].value : NULL;
    if (!sub) {
        out_string(c, "CLIENT_ERROR usage: mcz <stats|ns> ...");
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
       rc = dump_json_into_buf(&out, &len, ns ? ns : "global", &snap);
    } else {
       rc = dump_ascii_into_buf(&out, &len, ns ? ns : "global", &snap);
    }
    if (rc < 0) { out_string(c, "SERVER_ERROR memory allocation failed"); return; }
    if (!out) { out_string(c, "SERVER_ERROR serialization failed"); return; }

    write_buf(c, out, len);
    free(out);
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
    
    if (dump_ascii_into_buf(&payload, &plen, ns ? ns : "global", &snap) < 0) {
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
