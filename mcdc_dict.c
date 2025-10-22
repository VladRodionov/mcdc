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
 * mcdc_dict.c
 *
 * Dictionary metadata and routing table subsystem.
 *
 * Responsibilities:
 *   - Define metadata for individual dictionaries (mcdc_dict_meta_t).
 *   - Organize dictionaries into namespaces (mcdc_ns_entry_t).
 *   - Maintain the global router table (mcdc_table_t):
 *       • Lookup by namespace prefix.
 *       • O(1) lookup by ID via by_id[] array.
 *       • Sorted newest-first within each namespace.
 *   - Provide APIs for:
 *       • Parsing/writing manifest files.
 *       • Loading/saving dictionaries to the filesystem.
 *       • Publishing new tables atomically via copy-on-write.
 *
 * Design:
 *   - Copy-on-write: new table built from old + new dict, then atomically
 *     published.
 *   - Old tables retired to GC for safe reclamation.
 *   - Default namespace is "default" if none provided.
 *
 * Naming convention:
 *   - All functions/types prefixed with `mcdc_dict_*` belong to this subsystem.
 */
//* ---- macOS-friendly feature macros (must come before any system headers) ---- */
#if defined(__APPLE__)
  /* Expose Darwin extensions like sys_signame/NSIG/timegm */
  #ifndef _DARWIN_C_SOURCE
  #define _DARWIN_C_SOURCE 1
  #endif
  /* If strict POSIX was previously forced by another header, undo it here. */
  #undef _XOPEN_SOURCE
  /* Keeping _POSIX_C_SOURCE is fine as long as _DARWIN_C_SOURCE is also set. */
#endif

/* Keep POSIX 2008 features too (getline, etc.). Safe alongside _DARWIN_C_SOURCE. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* --------------------------------------------------------------------------- */
#include <strings.h>     /* strcasecmp on BSD/macOS */
#include <sys/signal.h>  /* defines NSIG/sys_signame on macOS */
#include <signal.h>
#include <time.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "mcdc_dict.h"
#include "mcdc_utils.h"
#include "mcdc_dict_pool.h"
#include "mcdc_stats.h"


/* ----- manifest parsing ----- */
static int parse_manifest_file(const char *mf_path, const char *dir, mcdc_dict_meta_t *m){
    memset(m, 0, sizeof(*m));
    FILE *fp = fopen(mf_path, "r");
    if (!fp) return -errno;
    m->mf_path = xstrdup(mf_path);

    char *line=NULL; size_t cap=0; int rc=0;
    while (getline(&line, &cap, fp) != -1) {
        trim(line);
        if (!*line || *line=='#') continue;
        char *eq = strchr(line,'=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq+1;
        trim(key); trim(val);

        if (!strcasecmp(key,"id")) {
            long v = strtol(val, NULL, 10);
            if (v<0 || v>65535) { rc = -EINVAL; break; }
            m->id = (uint16_t)v;

        } else if (!strcasecmp(key,"dict_file")) {
            char buf[PATH_MAX];
            if (val[0] == '/') m->dict_path = xstrdup(val);
            else { if (join_path(buf,sizeof(buf),dir,val)) { rc=-ENAMETOOLONG; break; } m->dict_path = xstrdup(buf); }

        } else if (!strcasecmp(key,"namespaces")) {
            char *tmp = xstrdup(val);
            if (!tmp) { rc = -ENOMEM; break; }
            rc = split_prefixes(tmp, &m->prefixes, &m->nprefixes);
            free(tmp);
            if (rc) break;

        } else if (!strcasecmp(key,"created")) {
            time_t t; if (parse_rfc3339_utc(val, &t)) { rc = -EINVAL; break; }
            m->created = t;

        } else if (!strcasecmp(key,"level")) {
            m->level = (int)strtol(val,NULL,10);

        } else if (!strcasecmp(key,"signature")) {
            m->signature = xstrdup(val);
        } else if (!strcasecmp(key,"retired")) {
            if (*val == '\0') { m->retired = 0; }
            else {
                time_t t;
                if (parse_rfc3339_utc(val, &t)) { rc = -EINVAL; break; }
                m->retired = t;
            }
        }
    }
    free(line);
    fclose(fp);
    if (rc) return rc;

    /* Default namespace if missing */
    if (!m->prefixes || m->nprefixes==0) {
        m->prefixes = calloc(1,sizeof(char*));
        if (!m->prefixes) return -ENOMEM;
        m->prefixes[0] = xstrdup("default");
        m->nprefixes = 1;
    }

    return 0;
}

/* Load .dict into memory and create shared CDict/DDict */
static int load_zstd_dict(const char *path, int level, const ZSTD_CDict **outC, const ZSTD_DDict **outD, size_t *sz_out){
    int rc = 0; *outC=NULL; *outD=NULL; if (sz_out) *sz_out=0;
    FILE *fp = fopen(path,"rb"); if (!fp) return -errno;
    if (fseek(fp,0,SEEK_END)!=0){ rc=-errno; goto out; }
    long n = ftell(fp); if (n<=0){ rc=-EINVAL; goto out; }
    rewind(fp);
    void *buf = malloc((size_t)n);
    if (!buf){ rc=-ENOMEM; goto out; }
    if (fread(buf,1,(size_t)n,fp) != (size_t)n){ free(buf); rc=-EIO; goto out; }

    const ZSTD_CDict *cd = ZSTD_createCDict(buf,(size_t)n, level > 0 ? level : 3);
    const ZSTD_DDict *dd = ZSTD_createDDict(buf,(size_t)n);
    /* dict buffer can be freed after CDict/DDict creation */
    free(buf);
    if (!cd || !dd) { if (cd) ZSTD_freeCDict((ZSTD_CDict*)cd); if (dd) ZSTD_freeDDict((ZSTD_DDict*)dd); rc = -EINVAL; goto out; }

    *outC = cd; *outD = dd; if (sz_out) *sz_out = (size_t)n;
out:
    fclose(fp);
    return rc;
}

/* Phase-1 signature checker stub */
static bool verify_manifest_signature(const mcdc_dict_meta_t *m){
    (void)m;
    /* TODO: implement (e.g., SHA256 of dict + fields). For now, accept. */
    return true;
}

/* Free one dict meta */
static void free_dict_meta(mcdc_dict_meta_t *m){
    if (!m) return;
    if (m->cdict) ZSTD_freeCDict((ZSTD_CDict*)m->cdict);
    if (m->ddict) ZSTD_freeDDict((ZSTD_DDict*)m->ddict);
    free(m->dict_path); free(m->mf_path); free(m->signature);
    if (m->prefixes){ for (size_t i=0;i<m->nprefixes;i++) free(m->prefixes[i]); free(m->prefixes); }
    memset(m,0,sizeof(*m));
}

/* ---- helpers ---- */

static mcdc_ns_entry_t *find_or_add_space(mcdc_ns_entry_t ***spaces, size_t *nspaces, size_t *cspaces, const char *pref) {
    for (size_t i=0;i<*nspaces;i++) {
        if (strcmp((*spaces)[i]->prefix, pref) == 0) return (*spaces)[i];
    }
    if (*nspaces == *cspaces) {
        size_t nc = *cspaces ? (*cspaces * 2) : 8;
        void *tmp = realloc(*spaces, nc * sizeof(**spaces));
        if (!tmp) return NULL;
        *spaces = tmp; *cspaces = nc;
    }
    mcdc_ns_entry_t *s = (mcdc_ns_entry_t*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->prefix = strdup(pref);
    (*spaces)[(*nspaces)++] = s;
    return s;
}

static int cmp_meta_created_desc(const void *a, const void *b) {
    const mcdc_dict_meta_t *A = *(mcdc_dict_meta_t * const*)a;
    const mcdc_dict_meta_t *B = *(mcdc_dict_meta_t * const*)b;
    if (A->created == B->created) return (int)B->id - (int)A->id; /* newer id first on tie */
    return (A->created < B->created) ? 1 : -1; /* newest first */
}

/* -------------------------
 * Part 1: write dictionary file
 * ------------------------- */

/* Writes <dir>/<uuid>.dict (UUID v4). Returns absolute path and dict size. */
static int mcdc_save_dict_file(const char *dir,
                       const void *dict_data, size_t dict_size,
                       char **out_abs_path,        /* optional */
                       char **out_dict_basename,   /* optional: "<uuid>.dict" */
                       size_t *out_dict_size,      /* optional */
                       char **err_out)
{
    if (!dir || !*dir || !dict_data || dict_size == 0) {
        set_err(err_out, "mcdc_save_dict_file: invalid arguments");
        return -EINVAL;
    }

    char dict_base[64];
    int rc = make_uuid_basename("dict", dict_base, err_out);
    if (rc) return rc;

    char dict_path[PATH_MAX];
    if (join_path(dict_path, sizeof(dict_path), dir, dict_base) != 0) {
        set_err(err_out, "mcdc_save_dict_file: path too long");
        return -ENAMETOOLONG;
    }

    rc = atomic_write_file(dir, dict_path, dict_data, dict_size, 0644, err_out);
    if (rc != 0) {
        if (!*err_out) set_err(err_out, "mcdc_save_dict_file: atomic_write_file failed");
        return rc;
    }

    if (out_abs_path) {
        *out_abs_path = strdup(dict_path);
        if (!*out_abs_path) { set_err(err_out, "mcdc_save_dict_file: OOM abs path"); return -ENOMEM; }
    }
    if (out_dict_basename) {
        *out_dict_basename = strdup(dict_base);
        if (!*out_dict_basename) { set_err(err_out, "mcdc_save_dict_file: OOM basename"); return -ENOMEM; }
    }
    if (out_dict_size) *out_dict_size = dict_size;

    return 0;
}

/* Build "a, b, c" or "default" if none. Caller must free(). */
static char *build_ns_line(const char * const *prefixes, size_t nprefixes) {
    if (!prefixes || nprefixes == 0) return strdup("default");
    size_t total = 1;
    for (size_t i = 0; i < nprefixes; ++i) total += strlen(prefixes[i]) + 2;
    char *s = (char*)malloc(total);
    if (!s) return NULL;
    s[0] = '\0';
    for (size_t i = 0; i < nprefixes; ++i) {
        if (i) strcat(s, ", ");
        strcat(s, prefixes[i]);
    }
    return s;
}

/* -------------------------
 * Part 2: write <dir>/<uuid>.mf
 *   - prefixes may be NULL/0 => "default"
 *   - signature optional (may be NULL/empty)
 *   - created: pass 0 to use current time
 * ------------------------- */

/* Renders manifest text */
static int render_manifest_text(const char *dict_basename,
                                uint16_t id,
                                const char *ns_line,
                                time_t created,
                                int level,
                                const char *signature,
                                time_t retired,
                                char **out_text,
                                char **err_out)
{
    char created_rfc3339[32];
    format_rfc3339_utc(created ? created : time(NULL), created_rfc3339);

    char retired_rfc3339[32] = {0};
    const char *retired_k = "";
    if (retired > 0) {
        format_rfc3339_utc(retired, retired_rfc3339);
        retired_k = "retired = ";
    }

    const char *sig = (signature && *signature) ? signature : NULL;

    size_t cap = 360 + strlen(dict_basename) + strlen(ns_line)
               + (sig ? strlen(sig) : 0)
               + (retired > 0 ? strlen(retired_rfc3339) : 0);

    char *buf = (char*)malloc(cap);
    if (!buf) {
        set_err(err_out, "manifest: OOM render");
        return -ENOMEM;
    }

    int n = snprintf(buf, cap,
        "# MC/DC dictionary manifest\n"
        "dict_file = %s\n"
        "namespaces = %s\n"
        "created = %s\n"
        "level = %d\n"
        "id = %u\n"
        "%s%s%s"
        "%s%s%s",
        dict_basename, ns_line, created_rfc3339, level, id,
        sig ? "signature = " : "", sig ? sig : "", sig ? "\n" : "",
        (retired > 0 ? retired_k : ""),
        (retired > 0 ? retired_rfc3339 : ""),
        (retired > 0 ? "\n" : ""));

    if (n < 0 || (size_t)n >= cap) {
        free(buf);
        set_err(err_out, "manifest: snprintf overflow");
        return -EOVERFLOW;
    }

    *out_text = buf;
    return 0;
}

/* Extract basename from absolute dict path. */
static const char *basename_from_path(const char *path){
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

/* Writes <dir>/<uuid>.mf that points to the given dict basename (<uuid>.dict).
   No ID is saved in the manifest (IDs are assigned during reload). */
static int mcdc_save_manifest_file(const char *dir,
                           const char *dict_basename,            /* "<uuid>.dict" */
                           const char * const *prefixes, size_t nprefixes,
                           int level,
                           const char *signature,                /* optional */
                           time_t created,                       /* 0 => now */
                           time_t retired,                       /* 0 => active */
                           char **out_abs_path,                  /* optional */
                           char **err_out)
{
    if (!dir || !*dir || !dict_basename || !*dict_basename) {
        set_err(err_out, "manifest: invalid args");
        return -EINVAL;
    }
    size_t dblen = strlen(dict_basename);
    if (dblen < 6 || strcmp(dict_basename + dblen - 5, ".dict") != 0) {
        set_err(err_out, "manifest: dict_basename must end with .dict");
        return -EINVAL;
    }

    /* ns line */
    char *ns_line = build_ns_line(prefixes, nprefixes);
    if (!ns_line) { set_err(err_out, "manifest: OOM namespaces"); return -ENOMEM; }

    /* render (no id line) */
    char *mf_text = NULL;
    int rc = render_manifest_text(dict_basename, 0, ns_line, created, level, signature, retired, &mf_text, err_out);
    if (rc) { free(ns_line); return rc; }

    /* manifest basename: same UUID, ".mf" */
    char mf_base[64];
    memcpy(mf_base, dict_basename, dblen - 5);
    mf_base[dblen - 5] = '\0';
    strncat(mf_base, ".mf", sizeof(mf_base) - (dblen - 5) - 1);

    char mf_path[PATH_MAX];
    if (join_path(mf_path, sizeof(mf_path), dir, mf_base) != 0) {
        free(ns_line); free(mf_text);
        set_err(err_out, "manifest: path too long");
        return -ENAMETOOLONG;
    }

    rc = atomic_write_text(dir, mf_path, mf_text, err_out);
    if (rc) { free(ns_line); free(mf_text); return rc; }

    if (out_abs_path) {
        *out_abs_path = strdup(mf_path);
        if (!*out_abs_path) { set_err(err_out, "manifest: OOM strdup path"); free(ns_line); free(mf_text); return -ENOMEM; }
    }

    free(ns_line);
    free(mf_text);
    return 0;
}

/* Rewrite the manifest at meta->mf_path using fields from meta.
 * - No `id =` line (IDs are assigned on reload).
 * - Uses dict basename from meta->dict_path for `dict_file = ...`.
 */
static int mcdc_rewrite_manifest(const mcdc_dict_meta_t *meta, char **err_out)
{
    if (!meta || !meta->mf_path || !*meta->mf_path || !meta->dict_path || !*meta->dict_path) {
        set_err(err_out, "manifest: invalid meta (missing mf_path/dict_path)");
        return -EINVAL;
    }

    /* namespaces line */
    char *ns_line = build_ns_line((const char * const*)meta->prefixes, meta->nprefixes);
    if (!ns_line) { set_err(err_out, "manifest: OOM namespaces"); return -ENOMEM; }

    /* dict basename for `dict_file = ...` */
    const char *dict_base = basename_from_path(meta->dict_path);

    /* render manifest text (no id= line) */
    char *mf_text = NULL;
    int rc = render_manifest_text(
                 dict_base,
                 meta->id,
                 ns_line,
                 meta->created ? meta->created : time(NULL),
                 meta->level,
                 meta->signature,
                 meta->retired,
                 &mf_text,
                 err_out);
    if (rc) { free(ns_line); return rc; }

    /* compute directory for atomic write from meta->mf_path */
    char *pathdup = strdup(meta->mf_path);
    if (!pathdup) { free(ns_line); free(mf_text); set_err(err_out, "manifest: OOM path dup"); return -ENOMEM; }

    char *slash = strrchr(pathdup, '/');
    if (slash) *slash = '\0';
    else strcpy(pathdup, "."); /* relative dir */

    /* write atomically to the absolute mf_path already provided */
    rc = atomic_write_text(pathdup, meta->mf_path, mf_text, err_out);

    free(pathdup);
    free(ns_line);
    free(mf_text);
    return rc;
}

static int assign_ids_from_fs(mcdc_dict_meta_t *metas, size_t n,
                           int64_t quarantine_s, char **err_out)
{
    bool used[65536] = {0};
    time_t now = time(NULL);

    /* 1) mark disallowed: active + retired within quarantine */
    for (size_t i = 0; i < n; ++i) {
        const mcdc_dict_meta_t *m = &metas[i];
        if (m->id == 0) continue;
        if (m->retired == 0 || (m->retired > 0 && (now - m->retired) < quarantine_s)) {
            used[m->id] = true;
        }
    }

    /* 2) assign ids for metas lacking one */
    for (size_t i = 0; i < n; ++i) {
        mcdc_dict_meta_t *m = &metas[i];
        if (m->id != 0) continue;

        uint16_t pick = 0;
        for (uint32_t id = 1; id <= 65535; ++id) {
            if (!used[id]) { pick = (uint16_t)id; used[id] = true; break; }
        }
        if (!pick) { set_err(err_out, "ID space exhausted"); return -ENOSPC; }

        m->id = pick;
        /* immediately persist manifest with assigned id */
        int rc = mcdc_rewrite_manifest((const mcdc_dict_meta_t *)m, err_out);
        if (rc) return rc;
    }
    return 0;
}

/*
 * Returns array of C strings with known namespace prefixes (except "default").
 * Count is returned in *count.
 *
 * IMPORTANT: returned array points into the existing ns_entry objects;
 *            caller must NOT free the strings, only the array if needed.
 */

static const char **list_namespaces(const mcdc_table_t *table, size_t *count) {
    if (count) *count = 0;
    if (!table || table->nspaces == 0)
        return NULL;

    const size_t n = table->nspaces;
    const char **list = calloc(n, sizeof(*list));
    if (!list)
        return NULL;

    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        mcdc_ns_entry_t *ns = table->spaces[i];
        if (!ns || !ns->prefix)
            continue;

        /* Skip "default" namespace */
        if (strcmp(ns->prefix, "default") == 0)
            continue;

        list[out] = strdup(ns->prefix);
        if (!list[out]) {
            /* On failure, free previous strings and the list */
            for (size_t j = 0; j < out; j++) free((void *)list[j]);
            free(list);
            return NULL;
        }
        out++;
    }

    if (out == 0) {  /* No entries after skipping defaults */
        free(list);
        return NULL;
    }

    if (count) *count = out;
    return list;      /* Caller must free each list[i] and then list */
}
/* ----------------- PUBLIC API ----------------- */


mcdc_table_t *mcdc_scan_dict_dir(const char *dir,
                               size_t max_per_ns,
                               int64_t id_quarantine_s,
                               int comp_level,
                               char **err_out)
{

    if (!dir || !*dir) { set_err(err_out, "mcdc_scan_dict_dir: empty dir"); return NULL; }
    if (max_per_ns == 0) max_per_ns = 1;

    DIR *d = opendir(dir);
    if (!d) { set_err(err_out, "mcdc_scan_dict_dir: opendir(%s) failed", dir); return NULL; }

    /* -------- Phase 1: collect metas from manifests  -------- */
    size_t nmeta = 0, cmeta = 8;
    mcdc_dict_meta_t *metas = (mcdc_dict_meta_t*)calloc(cmeta, sizeof(*metas));
    if (!metas) { closedir(d); set_err(err_out, "mcdc_scan_dict_dir: OOM spaces"); return NULL; }

    struct dirent *de;

    while ((de = readdir(d))) {
        const char *name = de->d_name;
        if (name[0] == '.') continue;
        size_t ln = strlen(name);
        if (ln > 3 && strcmp(name + ln - 3, ".mf") == 0) {
            if (nmeta == cmeta) {
                size_t nc = cmeta * 2;
                void *tmp = realloc(metas, nc * sizeof(*metas));
                if (!tmp) { nmeta = 0; break; }
                metas = (mcdc_dict_meta_t*)tmp; cmeta = nc;
            }

            char mfpath[PATH_MAX] = {0};
            if (join_path(mfpath, sizeof(mfpath), dir, name)) continue;

            mcdc_dict_meta_t *m = &metas[nmeta];
            memset(m, 0, sizeof(*m));
            if (parse_manifest_file(mfpath, dir, m) == 0) {
                nmeta++;
                /* Stub: to avoid warning during compilation */
                verify_manifest_signature(m);
            } else {
                free_dict_meta(m);
            }
        }
    }

    closedir(d);
    if (nmeta == 0) { free(metas); set_err(err_out, "mcdc_scan_dict_dir: no manifests"); return NULL; }

    /* -------- Phase 2: assign IDs (filesystem is source of truth) -------- */
    {
        int rc = assign_ids_from_fs(metas, nmeta, id_quarantine_s, err_out);
        if (rc != 0) {
            for (size_t i=0;i<nmeta;i++) free_dict_meta(&metas[i]);
            free(metas);
            return NULL;
        }
    }

    /* -------- Phase 3: group by namespace, sort newest-first -------- */
    size_t nspaces = 0, cspaces = 8;
    mcdc_ns_entry_t **spaces = (mcdc_ns_entry_t**)calloc(cspaces, sizeof(*spaces));
    if (!spaces) {
        for (size_t i=0;i<nmeta;i++) free_dict_meta(&metas[i]);
        free(metas);
        set_err(err_out, "mcdc_scan_dict_dir: OOM spaces");
        return NULL;
    }

    for (size_t i=0;i<nmeta;i++) {
        mcdc_dict_meta_t *m = &metas[i];
        size_t npre = (m->nprefixes && m->prefixes) ? m->nprefixes : 1;
        for (size_t j=0;j<npre;j++) {
            const char *pref = (m->prefixes && m->prefixes[j]) ? m->prefixes[j] : "default";
            mcdc_ns_entry_t *sp = find_or_add_space(&spaces, &nspaces, &cspaces, pref);
            if (!sp) continue;
            size_t nd = sp->ndicts;
            void *tmp = realloc(sp->dicts, (nd+1) * sizeof(*sp->dicts));
            if (!tmp) continue;
            sp->dicts = (mcdc_dict_meta_t**)tmp;
            sp->dicts[nd] = m;
            sp->ndicts = nd + 1;
        }
    }

    for (size_t i=0;i<nspaces;i++) {
        if (spaces[i]->ndicts > 1) {
            qsort(spaces[i]->dicts, spaces[i]->ndicts,
                  sizeof(spaces[i]->dicts[0]), cmp_meta_created_desc);
        }
    }

    /* -------- Phase 4: enforce per-namespace limit via retirement -------- */
    time_t now = time(NULL);
    for (size_t i=0;i<nspaces;i++) {
        mcdc_ns_entry_t *sp = spaces[i];
        size_t kept_active = 0;
        for (size_t k = 0; k < sp->ndicts; ++k) {
            mcdc_dict_meta_t *m = sp->dicts[k];
            if (m->retired == 0) {
                if (kept_active < max_per_ns) {
                    kept_active++;
                } else {
                    int32_t ref_left = -1;
                    mcdc_dict_pool_release_for_meta(m, &ref_left, err_out);
                    if (ref_left == 0) {
                        /* retire overflow (persist to manifest) */
                        mcdc_mark_dict_retired(m, now, err_out);
                    }
                }
            }
        }
    }

    /* -------- Phase 5: load ZSTD dicts for ACTIVE metas only -------- */
    size_t n_active = 0;
    for (size_t i=0;i<nmeta;i++) if (metas[i].retired == 0) n_active++;

    if (n_active == 0) {
        for (size_t i=0;i<nspaces;i++) { free(spaces[i]->dicts); free(spaces[i]->prefix); free(spaces[i]); }
        for (size_t i=0;i<nmeta;i++) free_dict_meta(&metas[i]);
        free(spaces); free(metas);
        set_err(err_out, "mcdc_scan_dict_dir: no active dictionaries after limit enforcement");
        return NULL;
    }

    bool failed_load_some = false;
    for (size_t i=0;i<nmeta;i++) {
        mcdc_dict_meta_t *m = &metas[i];
        if (m->retired != 0) continue;
        size_t dsz = 0;
        if (load_zstd_dict(m->dict_path, comp_level, &m->cdict, &m->ddict, &dsz) == 0) {
            m->dict_size = dsz;
            // As a side effect this function can update both: CDict and DDict references
            mcdc_dict_pool_retain_for_meta(m, err_out);
        } else {
            if (!failed_load_some) {
                failed_load_some = true;
                set_err(err_out, "mcdc_scan_dict_dir: dict load failed. Check the server's log for details");
            }
            /* If load fails, retire and persist so FS remains the truth, log error even if verbose = 1 */
            fprintf(stderr, "[mcz-scan-dict-dir] failed to load %s", m->dict_path);
            mcdc_mark_dict_retired(m, now, err_out);
        }
    }

    /* -------- Phase 6: build final table (ACTIVE only) -------- */
    mcdc_table_t *tab = (mcdc_table_t*)calloc(1, sizeof(*tab));
    if (!tab) {
        for (size_t i=0;i<nspaces;i++) { free(spaces[i]->dicts); free(spaces[i]->prefix); free(spaces[i]); }
        for (size_t i=0;i<nmeta;i++) free_dict_meta(&metas[i]);
        free(spaces); free(metas);
        set_err(err_out, "mcdc_scan_dict_dir: OOM table");
        return NULL;
    }

    for (size_t i=0;i<65536;i++) tab->by_id[i] = NULL;

    /* Filter spaces to only active metas */
    for (size_t i=0;i<nspaces;i++) {
        mcdc_ns_entry_t *sp = spaces[i];
        size_t w = 0;
        for (size_t k=0;k<sp->ndicts;k++) {
            mcdc_dict_meta_t *m = sp->dicts[k];
            if (m->retired == 0) {
                sp->dicts[w++] = m;
                /* by_id: newest wins */
                mcdc_dict_meta_t *cur = tab->by_id[m->id];
                if (!cur || m->created > cur->created) tab->by_id[m->id] = m;
            }
        }
        sp->ndicts = w;
    }

    tab->spaces   = spaces;
    tab->nspaces  = nspaces;
    tab->metas    = metas;
    tab->nmeta    = nmeta;
    tab->built_at = now;
    size_t ns_sz = 0;
    const char ** ns_list = list_namespaces(tab, &ns_sz);
    mcdc_stats_rebuild_from_list(ns_list, ns_sz, 0);
    return tab;
}

/* Find the next available dictionary ID, considering quarantine on retired IDs.
 * - metas: array of already-parsed manifest metas (filesystem is source of truth)
 * - n: number of metas
 * - quarantine_s: seconds; IDs with retired!=0 and (now - retired) < quarantine_s are disallowed
 * - out_id: on success, set to the chosen ID in [1..65535]
 * Returns 0 on success; -ENOSPC if no ID free; -EINVAL on bad args.
 */
int mcdc_next_available_id(const mcdc_dict_meta_t *metas, size_t n,
                          int64_t quarantine_s, uint16_t *out_id, char **err_out)
{
    if (!out_id) { set_err(err_out, "next_id: out_id is NULL"); return -EINVAL; }

    bool used[65536];
    memset(used, 0, sizeof(used));

    time_t now = time(NULL);

    /* Mark disallowed IDs: active OR retired but still within quarantine window. */
    if (metas && n) {
        for (size_t i = 0; i < n; ++i) {
            const mcdc_dict_meta_t *m = &metas[i];
            if (m->retired == 0 || (m->retired > 0 && (now - m->retired) < quarantine_s)) {
                used[m->id] = true;
            }
        }
    }

    /* Pick the lowest free ID in [1..65535]. */
    for (uint32_t id = 1; id <= 65535; ++id) {
        if (!used[id]) {
            *out_id = (uint16_t)id;
            return 0;
        }
    }

    set_err(err_out, "next_id: no free IDs (all 1..65535 are active or quarantined)");
    return -ENOSPC;
}

/* Pick the active dict for a key (Phase-1: longest prefix match; falls back to "default") */
const mcdc_dict_meta_t *mcdc_pick_dict(const mcdc_table_t *tab, const char *key, size_t klen){
    if (!tab || !key) return NULL;
    const mcdc_dict_meta_t *fallback = NULL;
    for (size_t i=0;i<tab->nspaces;i++) {
        mcdc_ns_entry_t *sp = tab->spaces[i];
        if (!sp->ndicts) continue;
        if (!strcmp(sp->prefix,"default")) { fallback = sp->dicts[0]; continue; }
        size_t plen = strlen(sp->prefix);
        if (plen <= klen && !strncmp(key, sp->prefix, plen)) {
            return sp->dicts[0];
        }
    }
    return fallback;
}

bool mcdc_is_default_ns(const mcdc_table_t *tab, const char *key, size_t klen){
    if(!tab || !key) return false;
    const mcdc_dict_meta_t *meta = mcdc_pick_dict(tab, key, klen);
    if(meta){
        return (meta->nprefixes == 1 && strcmp(meta->prefixes[0], "default") == 0);
    } else {
        return true; // no meta yet
    }
}

bool mcdc_has_default_dict(const mcdc_table_t *tab){

    if (!tab) {
        return false;
    }
    for (size_t i=0;i<tab->nspaces;i++) {
        mcdc_ns_entry_t *sp = tab->spaces[i];
        if (!sp->ndicts) continue;
        if (!strcmp(sp->prefix,"default")) {
            return true;
        }
    }
    return false;
}

const mcdc_dict_meta_t *mcdc_lookup_by_id(const mcdc_table_t *tab, uint16_t id) {
    if (!tab) return NULL;
    return tab->by_id[id];
}


/* ------------------------------------------
 * Optional convenience wrapper:
 *   - calls both save functions
 *   - fills out_meta (paths, times, level, namespaces, signature, size)
 *   - does not create cdict/ddict here
 * ------------------------------------------ */
int mcdc_save_dictionary_and_manifest(const char *dir,
                                     const void *dict_data, size_t dict_size,
                                     const char * const *prefixes, size_t nprefixes,
                                     int level,
                                     const char *signature,
                                     time_t created,      /* 0 => now */
                                     time_t retired,      /* 0 => active */
                                     mcdc_dict_meta_t *out_meta,
                                     char **err_out)
{
    if (!dir || !*dir || !dict_data || dict_size == 0) {
        set_err(err_out, "mcdc_save_dictionary_and_manifest: invalid arguments");
        return -EINVAL;
    }

    char *dict_abs = NULL;
    char *dict_base = NULL;   /* "<uuid>.dict" */
    char *mf_abs = NULL;
    size_t saved_size = 0;

    /* 1) Save the dictionary bytes to <dir>/<uuid>.dict */
    int rc = mcdc_save_dict_file(dir,
                                dict_data, dict_size,
                                &dict_abs,           /* out abs path */
                                &dict_base,          /* out "<uuid>.dict" */
                                &saved_size,
                                err_out);
    if (rc) goto fail;

    /* Use a consistent timestamp if caller passed 0 */
    time_t ts_created = created ? created : time(NULL);

    /* 2) Save the manifest <dir>/<uuid>.mf pointing to dict_base */
    rc = mcdc_save_manifest_file(dir,
                                dict_base,
                                prefixes, nprefixes,
                                level,
                                signature,
                                ts_created,
                                retired,
                                &mf_abs,
                                err_out);
    if (rc) goto fail;

    /* 3) Fill out_meta (without ID; assigned at reload) */
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
        out_meta->id        = 0;        /* assigned later during reload */
        out_meta->dict_path = dict_abs; /* take ownership */
        out_meta->mf_path   = mf_abs;   /* take ownership */
        out_meta->created   = ts_created;
        out_meta->retired   = retired;
        out_meta->level     = level;
        out_meta->dict_size = saved_size;

        /* copy namespaces */
        if (!prefixes || nprefixes == 0) {
            out_meta->prefixes = (char**)calloc(1, sizeof(char*));
            if (!out_meta->prefixes) { set_err(err_out, "mcdc_save_dictionary_and_manifest: OOM prefixes"); rc = -ENOMEM; goto fail_free_temp; }
            out_meta->prefixes[0] = strdup("default");
            if (!out_meta->prefixes[0]) { set_err(err_out, "mcdc_save_dictionary_and_manifest: OOM prefix dup"); rc = -ENOMEM; goto fail_free_temp; }
            out_meta->nprefixes = 1;
        } else {
            out_meta->prefixes = (char**)calloc(nprefixes, sizeof(char*));
            if (!out_meta->prefixes) { set_err(err_out, "mcdc_save_dictionary_and_manifest: OOM prefixes"); rc = -ENOMEM; goto fail_free_temp; }
            out_meta->nprefixes = nprefixes;
            for (size_t i = 0; i < nprefixes; ++i) {
                out_meta->prefixes[i] = strdup(prefixes[i]);
                if (!out_meta->prefixes[i]) { set_err(err_out, "mcdc_save_dictionary_and_manifest: OOM prefix dup"); rc = -ENOMEM; goto fail_free_temp; }
            }
        }

        if (signature && *signature) {
            out_meta->signature = strdup(signature);
            if (!out_meta->signature) { set_err(err_out, "mcdc_save_dictionary_and_manifest: OOM signature"); rc = -ENOMEM; goto fail_free_temp; }
        }

        /* cdict/ddict are not created/owned here */
        out_meta->cdict = NULL;
        out_meta->ddict = NULL;
    } else {
        /* Caller didn’t want meta; free temp paths we allocated */
        free(dict_abs);
        free(mf_abs);
    }

    /* done; free transient basename buffer */
    free(dict_base);
    return 0;

fail_free_temp:
    /* If out_meta partially filled, caller's usual free_dict_meta() will clean it up;
       here we only free transient locals on error */
    if (!out_meta) {
        free(dict_abs);
        free(mf_abs);
    }
fail:
    free(dict_base);
    return rc;
}

/* Deep-copy a meta (strings + prefixes); keep cdict/ddict pointers as given */
static void meta_deep_copy(mcdc_dict_meta_t *dst, const mcdc_dict_meta_t *src,
                           const ZSTD_CDict *cdict, const ZSTD_DDict *ddict)
{
    memset(dst, 0, sizeof(*dst));
    dst->id         = src->id;
    dst->dict_path  = xstrdup(src->dict_path);
    dst->mf_path    = xstrdup(src->mf_path);
    dst->created    = src->created;
    dst->retired    = src->retired;
    dst->level      = src->level;
    dst->signature  = xstrdup(src->signature);
    dst->dict_size  = src->dict_size;
    dst->cdict      = cdict ? cdict : src->cdict;
    dst->ddict      = ddict ? ddict : src->ddict;

    if (src->nprefixes && src->prefixes) {
        dst->prefixes = (char**)calloc(src->nprefixes, sizeof(char*));
        if (dst->prefixes) {
            dst->nprefixes = src->nprefixes;
            for (size_t i = 0; i < src->nprefixes; ++i)
                dst->prefixes[i] = xstrdup(src->prefixes[i]);
        }
    } else {
        dst->prefixes = (char**)calloc(1, sizeof(char*));
        if (dst->prefixes) {
            dst->prefixes[0] = strdup("default");
            dst->nprefixes = 1;
        }
    }
}


/* push meta pointer to the *end*; we will ensure newest-first later, or we can insert at front */
static int space_append_meta(mcdc_ns_entry_t *sp, mcdc_dict_meta_t *m) {
    mcdc_dict_meta_t **newv = (mcdc_dict_meta_t**)realloc(sp->dicts, (sp->ndicts + 1) * sizeof(*newv));
    if (!newv) return -ENOMEM;
    sp->dicts = newv;
    sp->dicts[sp->ndicts++] = m;
    return 0;
}


/* Build a brand-new table = deep copy of old + one extra meta (as newest),
 * enforcing max_per_ns per-namespace (0 => unlimited).
 */

static mcdc_ns_entry_t *find_space(mcdc_ns_entry_t **spaces, size_t n, const char *pref) {
    for (size_t i = 0; i < n; ++i) {
        if (spaces[i] && spaces[i]->prefix && strcmp(spaces[i]->prefix, pref) == 0) return spaces[i];
    }
    return NULL;
}

static mcdc_ns_entry_t *add_space(mcdc_table_t *tab, const char *pref) {
    /* grow spaces array by 1 */
    mcdc_ns_entry_t **newv = (mcdc_ns_entry_t**)realloc(tab->spaces, (tab->nspaces + 1) * sizeof(*newv));
    if (!newv) return NULL;
    tab->spaces = newv;

    mcdc_ns_entry_t *sp = (mcdc_ns_entry_t*)calloc(1, sizeof(*sp));
    if (!sp) return NULL;
    sp->prefix = strdup(pref ? pref : "default");
    sp->dicts  = NULL;
    sp->ndicts = 0;

    tab->spaces[tab->nspaces++] = sp;
    return sp;
}

mcdc_table_t *table_clone_plus(const mcdc_table_t *old,
                                     const mcdc_dict_meta_t *new_meta_in,
                                     const ZSTD_CDict *cdict,
                                     const ZSTD_DDict *ddict,
                                     size_t max_per_ns,
                                     char **err_out)
{
    mcdc_table_t *tab = (mcdc_table_t*)calloc(1, sizeof(*tab));
    if (!tab) { set_err(err_out, "table_clone_plus: %s", "OOM: table"); return NULL; }

    const size_t n_old = old ? old->nmeta : 0;
    tab->metas = (mcdc_dict_meta_t*)calloc(n_old + 1, sizeof(mcdc_dict_meta_t));
    if (!tab->metas) { set_err(err_out,"table_clone_plus: %s", "OOM: metas"); free(tab); return NULL; }
    tab->nmeta = n_old + 1;

    /* Copy old metas first */
    for (size_t i = 0; i < n_old; ++i) {
        meta_deep_copy(&tab->metas[i], &old->metas[i], old->metas[i].cdict, old->metas[i].ddict);
    }
    /* Append new meta as the newest */
    meta_deep_copy(&tab->metas[n_old], new_meta_in, cdict, ddict);

    /* by_id: newest wins (new meta last; iterate forward so it overwrites) */
    for (size_t i = 0; i < 65536; ++i) tab->by_id[i] = NULL;
    for (size_t i = 0; i < tab->nmeta; ++i) {
        tab->by_id[ tab->metas[i].id ] = &tab->metas[i];
    }

    /* rebuild spaces from scratch */
    tab->spaces = NULL;
    tab->nspaces = 0;
    for (size_t i = 0; i < tab->nmeta; ++i) {
        mcdc_dict_meta_t *m = &tab->metas[i];
        for (size_t j = 0; j < m->nprefixes; ++j) {
            const char *pref = m->prefixes[j] ? m->prefixes[j] : "default";
            mcdc_ns_entry_t *sp = find_space(tab->spaces, tab->nspaces, pref);
            if (!sp) {
                sp = add_space(tab, pref);
                if (!sp) { set_err(err_out, "table_clone_plus: %s", "OOM: space"); /* leak-safe clean later */ goto fail; }
            }
            if (space_append_meta(sp, m) != 0) { set_err(err_out, "table_clone_plus: %s", "OOM: space dicts"); goto fail; }
        }
    }

    /* newest-first per namespace, then enforce max_per_ns (trim oldest) */
    for (size_t i = 0; i < tab->nspaces; ++i) {
        mcdc_ns_entry_t *sp = tab->spaces[i];
        qsort(sp->dicts, sp->ndicts, sizeof(sp->dicts[0]), cmp_meta_created_desc);

        if (max_per_ns > 0 && sp->ndicts > max_per_ns) {
            /* Keep newest max_per_ns; drop the tail. We only shrink the view.
               (Metas stay owned by tab->metas; no frees needed here.) */
            sp->ndicts = max_per_ns;
            /* Optional: shrink capacity (not required)
               sp->dicts = realloc(sp->dicts, sp->ndicts * sizeof(*sp->dicts));
               (ignore realloc failure; it’s just a capacity hint) */
        }
    }

    tab->built_at = time(NULL);
    tab->gen      = old ? (old->gen + 1) : 1;
    return tab;

fail:
    /* best-effort cleanup */
    if (tab) {
        if (tab->spaces) {
            for (size_t i = 0; i < tab->nspaces; ++i) {
                if (tab->spaces[i]) {
                    free(tab->spaces[i]->dicts);
                    free(tab->spaces[i]->prefix);
                    free(tab->spaces[i]);
                }
            }
            free(tab->spaces);
        }
        if (tab->metas) {
            for (size_t i = 0; i < tab->nmeta; ++i) {
                free(tab->metas[i].dict_path);
                free(tab->metas[i].mf_path);
                if (tab->metas[i].prefixes) {
                    for (size_t j = 0; j < tab->metas[i].nprefixes; ++j) free(tab->metas[i].prefixes[j]);
                    free(tab->metas[i].prefixes);
                }
                free(tab->metas[i].signature);
            }
            free(tab->metas);
        }
        free(tab);
    }
    return NULL;
}

/* Mark a dictionary as retired at 'now' and rewrite its manifest.
   If already retired (retired!=0), it is left as-is. */
int mcdc_mark_dict_retired(mcdc_dict_meta_t *meta, time_t now, char **err_out)
{
    if (!meta || !meta->dict_path || meta->id == 0) {
        set_err(err_out, "retire: invalid meta");
        return -EINVAL;
    }
    if (meta->retired != 0) return 0;  /* already retired */

    meta->retired = (now ? now : time(NULL));
    return mcdc_rewrite_manifest(meta, err_out);
}

