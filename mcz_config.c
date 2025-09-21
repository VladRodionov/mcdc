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
 * mcz_config.c
 *
 * Configuration parsing and runtime options for memcached-Zstd (MCZ).
 *
 * Responsibilities:
 *   - Define configuration structures (compression level, dictionary paths, limits).
 *   - Parse configuration files, environment variables, or command-line arguments.
 *   - Provide getters for other modules (compression, dict, trainer).
 *
 * Design:
 *   - Centralized configuration, immutable after initialization.
 *   - Plain C structures, minimal dependencies.
 *   - All symbols prefixed with `mcz_` for consistency.
 */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memcached.h"                  /* settings, mcz_cfg_t          */
#include "mcz_config.h"

/* Trim helpers unchanged ... */

/* size parser: accepts K,KB,KiB,M,MB,MiB,G,GB,GiB (case-insensitive) */
static int parse_bytes(const char *val, int64_t *out) {
    if (!val || !*val) return -EINVAL;
    char *end;
    errno = 0;
    double v = strtod(val, &end);
    if (val == end) return -EINVAL;

    // consume optional whitespace
    while (*end && isspace((unsigned char)*end)) end++;

    int64_t mul = 1;
    if (*end) {
        char suf[8] = {0};
        size_t n = 0;
        while (*end && n < sizeof(suf)-1 && isalpha((unsigned char)*end)) {
            suf[n++] = tolower((unsigned char)*end++);
        }
        suf[n] = '\0';
        if (n == 0) return -EINVAL;

        if (!strcmp(suf,"k") || !strcmp(suf,"kb")) mul = 1024LL;
        else if (!strcmp(suf,"kib")) mul = 1024LL;
        else if (!strcmp(suf,"m") || !strcmp(suf,"mb")) mul = 1024LL*1024LL;
        else if (!strcmp(suf,"mib")) mul = 1024LL*1024LL;
        else if (!strcmp(suf,"g") || !strcmp(suf,"gb")) mul = 1024LL*1024LL*1024LL;
        else if (!strcmp(suf,"gib")) mul = 1024LL*1024LL*1024LL;
        else return -EINVAL;
        // trailing junk?
        while (*end && isspace((unsigned char)*end)) end++;
        if (*end) return -EINVAL;
    }
    long double tot = (long double)v * (long double)mul;
    if (tot < 0 || tot > (long double)INT64_MAX) return -ERANGE;
    *out = (int64_t)tot;
    return 0;
}

/* duration parser: plain number = seconds; accepts s/m/h suffix */
static int parse_duration_sec(const char *val, int64_t *out) {
    if (!val || !*val) return -EINVAL;
    char *end;
    errno = 0;
    double v = strtod(val, &end);
    if (val == end) return -EINVAL;
    while (*end && isspace((unsigned char)*end)) end++;
    int64_t mul = 1;
    if (*end) {
        char c = tolower((unsigned char)*end++);
        if (c == 's') mul = 1;
        else if (c == 'm') mul = 60;
        else if (c == 'h') mul = 3600;
        else return -EINVAL;
        while (*end && isspace((unsigned char)*end)) end++;
        if (*end) return -EINVAL;
    }
    long double tot = (long double)v * (long double)mul;
    if (tot < 0 || tot > (long double)INT64_MAX) return -ERANGE;
    *out = (int64_t)tot;
    return 0;
}

/* boolean parser */
static int parse_bool(const char *val, bool *out) {
    if (!val) return -EINVAL;
    if (!strcasecmp(val,"true") || !strcasecmp(val,"yes") || !strcasecmp(val,"on") || !strcmp(val,"1")) { *out=true;  return 0; }
    if (!strcasecmp(val,"false")|| !strcasecmp(val,"no")  || !strcasecmp(val,"off")|| !strcmp(val,"0")) { *out=false; return 0; }
    return -EINVAL;
}

/* fraction parser: 0..1 */
static int parse_frac(const char *val, double *out) {
    if (!val) return -EINVAL;
    char *end;
    errno = 0;
    double d = strtod(val, &end);
    if (val == end) return -EINVAL;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end) return -EINVAL;
    if (d < 0.0 || d > 1.0) return -ERANGE;
    *out = d;
    return 0;
}

/* key mode: raw | prefix:N | hash */
static int parse_key_mode(const char *val, int *mode, int *n_out) {
    if (!val) return -EINVAL;
    if (!strcasecmp(val,"raw"))  { *mode = MCZ_KEYMODE_RAW; *n_out = 0; return 0; }
    if (!strcasecmp(val,"hash")) { *mode = MCZ_KEYMODE_HASH; *n_out = 0; return 0; }
    if (!strncasecmp(val,"prefix:",7)) {
        const char *p = val + 7;
        if (!*p) return -EINVAL;
        char *end; long n = strtol(p, &end, 10);
        if (p == end || *end || n <= 0 || n > 4096) return -EINVAL;
        *mode = MCZ_KEYMODE_PREFIX; *n_out = (int)n; return 0;
    }
    return -EINVAL;
}

static void ltrim(char **s)
{
    while (isspace((unsigned char)**s)) (*s)++;
}

static void rtrim(char *s)
{
    for (char *p = s + strlen(s) - 1; p >= s && isspace((unsigned char)*p); p--)
        *p = '\0';
}


#ifdef USE_ZSTD
/*-------------------------------------------------------------------------*/
int parse_mcz_config(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "zstd: cannot open %s: %s\n", path, strerror(errno));
        return -errno;
    }

    char  *line = NULL;
    size_t cap  = 0;
    int    rc   = 0;
    int    ln   = 0;

    while (getline(&line, &cap, fp) != -1) {
        ++ln;
        char *p = line;
        ltrim(&p);
        if (*p == '\0' || *p == '#') continue;          /* blank / comment */

        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: missing '='\n", path, ln);
            rc = rc ? rc: -EINVAL;
            continue;
        }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        rtrim(key);
        ltrim(&val);
        rtrim(val);

        /* --- dispatch -------------------------------------------------- */
        /* Back-compat legacy keys (keep but warn) */
        if (strcasecmp(key, "level") == 0 || strcasecmp(key, "mcz.level") == 0) {
            char *end; errno = 0; long lvl = strtol(val, &end, 10);
            if (end == val || *end || errno) { fprintf(stderr, "%s:%d: invalid level '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            if (lvl < 1 || lvl > 22) { fprintf(stderr, "%s:%d: level %ld out of range (1-22)\n", path, ln, lvl); rc = rc?rc:-ERANGE; continue; }
            settings.zstd_level = (int)lvl;

        } else if (strcasecmp(key, "max_dict") == 0 || strcasecmp(key, "mcz.dict_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad dict size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_dict_size = (size_t)v;
            if (!strcasecmp(key,"max_dict")) fprintf(stderr, "%s:%d: NOTE: 'max_dict' is deprecated; use 'mcz.dict_size'\n", path, ln);

        } else if (strcasecmp(key, "min_train_size") == 0 || strcasecmp(key, "mcz.min_training_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad min_training_size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_min_training_size = (size_t)v;
            if (!strcasecmp(key,"min_train_size")) fprintf(stderr, "%s:%d: NOTE: 'min_train_size' is deprecated; use 'mcz.min_training_size'\n", path, ln);

        } else if (strcasecmp(key, "min_comp_size") == 0 || strcasecmp(key, "mcz.min_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad min_size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_min_size = (size_t)v;

        } else if (strcasecmp(key, "max_comp_size") == 0 || strcasecmp(key, "mcz.max_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad max_size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_max_size = (size_t)v;

        } else if (strcasecmp(key, "dict_dir_path") == 0 || strcasecmp(key, "mcz.dict_dir") == 0) {
            free(settings.mcz_dict_dir); settings.mcz_dict_dir = val && *val ? strdup(val) : NULL;
            if (!strcasecmp(key,"dict_dir_path")) fprintf(stderr, "%s:%d: NOTE: 'dict_dir_path' is deprecated; use 'mcz.dict_dir'\n", path, ln);

        } else if (strcasecmp(key, "disable_dict") == 0 || strcasecmp(key, "mcz.enable_dict") == 0) {
            if (!strcasecmp(key, "disable_dict")) {
                bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
                settings.mcz_enable_dict = !b;
                fprintf(stderr, "%s:%d: NOTE: 'disable_dict' is deprecated; use 'mcz.enable_dict'\n", path, ln);
            } else {
                bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
                settings.mcz_enable_dict = b;
            }

        } else if (strcasecmp(key, "disable_comp") == 0 || strcasecmp(key, "mcz.enable_comp") == 0) {
            if (!strcasecmp(key, "disable_comp")) {
                bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
                settings.mcz_enable_comp = !b;
                fprintf(stderr, "%s:%d: NOTE: 'disable_comp' is deprecated; use 'mcz.enable_comp'\n", path, ln);
            } else {
                bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
                settings.mcz_enable_comp = b;
            }

        } else if (strcasecmp(key, "mcz.min_savings") == 0) {
            double d; if (parse_frac(val, &d)) { fprintf(stderr, "%s:%d: bad min_savings '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_min_savings = d;
            
            /* Training */
        } else if (strcasecmp(key, "mcz.enable_training") == 0) {
            bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_enable_training = b;

        } else if (strcasecmp(key, "mcz.retraining_interval") == 0) {
            int64_t s; if (parse_duration_sec(val, &s)) { fprintf(stderr, "%s:%d: bad retraining_interval '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_retraining_interval_s = s;

        } else if (strcasecmp(key, "mcz.min_training_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad min_training_size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_min_training_size = (size_t)v;

        } else if (strcasecmp(key, "mcz.ewma_alpha") == 0) {
            double d; if (parse_frac(val, &d)) { fprintf(stderr, "%s:%d: bad ewma_alpha '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_ewma_alpha = d;

        } else if (strcasecmp(key, "mcz.retrain_drop") == 0) {
            double d; if (parse_frac(val, &d)) { fprintf(stderr, "%s:%d: bad retrain_drop '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_retrain_drop = d;
        } else if (strcasecmp(key, "mcz.gc_run_interval") == 0) {
                int64_t s; if (parse_duration_sec(val, &s)) { fprintf(stderr, "%s:%d: bad gc_run_interval '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
                settings.mcz_gc_run_interval = s;
            
        } else if (strcasecmp(key, "mcz.gc_cool_period") == 0) {
            int64_t s; if (parse_duration_sec(val, &s)) { fprintf(stderr, "%s:%d: bad gc_cool_period '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_gc_cool_period = s;
            
        } else if (strcasecmp(key, "mcz.gc_quarantine_period") == 0) {
               int64_t s; if (parse_duration_sec(val, &s)) { fprintf(stderr, "%s:%d: bad gc_quarantine_period '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
               settings.mcz_gc_quarantine_period = s;
        /* Retention */
        } else if (strcasecmp(key, "mcz.dict_retain_hours") == 0) {
            char *end; long v = strtol(val, &end, 10);
            if (val == end || *end || v < 0 || v > 24*365) { fprintf(stderr, "%s:%d: bad dict_retain_hours '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_dict_retain_hours = (int)v;

        } else if (strcasecmp(key, "mcz.dict_retain_max") == 0) {
            char *end; long v = strtol(val, &end, 10);
            if (val == end || *end || v < 1 || v > 256) { fprintf(stderr, "%s:%d: bad dict_retain_max '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_dict_retain_max = (int)v;

            /* Sampling + Spool */
        } else if (strcasecmp(key, "mcz.enable_sampling") == 0) {
            bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_enable_sampling = b;

        } else if (strcasecmp(key, "mcz.sample_p") == 0) {
            double d; if (parse_frac(val, &d)) { fprintf(stderr, "%s:%d: bad sample_p '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_sample_p = d;

        } else if (strcasecmp(key, "mcz.sample_window_sec") == 0) {
            char *end; long v = strtol(val, &end, 10);
            if (val == end || *end || v < 1 || v > (24*3600)) { fprintf(stderr, "%s:%d: bad sample_window_sec '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_sample_window_sec = (int)v;

        } else if (strcasecmp(key, "mcz.sample_roll_bytes") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad sample_roll_bytes '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_sample_roll_bytes = (size_t)v;

        } else if (strcasecmp(key, "mcz.spool_dir") == 0) {
            free(settings.mcz_spool_dir); settings.mcz_spool_dir = val && *val ? strdup(val) : NULL;

        } else if (strcasecmp(key, "mcz.spool_max_bytes") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad spool_max_bytes '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_spool_max_bytes = (size_t)v;

        } else if (strcasecmp(key, "mcz.sample_key_mode") == 0) {
            int mode, n; if (parse_key_mode(val, &mode, &n)) { fprintf(stderr, "%s:%d: bad sample_key_mode '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            settings.mcz_sample_key_mode = mode; settings.mcz_sample_prefix_n = n;

            /* Unknown */
        } else if (strcasecmp(key, "compress_keys") == 0) {
            /* legacy: ignored in MCZ; accept to avoid breaking configs */
            fprintf(stderr, "%s:%d: NOTE: 'compress_keys' ignored\n", path, ln);

        } else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, ln, key);
            /* not fatal; continue */
        }
    }
    /* basic sanity checks */
    if (settings.mcz_min_size > settings.mcz_max_size) {
        fprintf(stderr, "mcz: min_size > max_size\n"); rc = rc?rc:-EINVAL;
    }
    if (settings.mcz_enable_sampling && (settings.mcz_sample_p <= 0.0 || settings.mcz_sample_p > 1.0)) {
        fprintf(stderr, "mcz: sample_p must be in (0,1]\n"); rc = rc?rc:-ERANGE;
    }
    free(line);
    fclose(fp);
    return rc;      /* 0 if perfect, first fatal errno otherwise */
}
#endif
