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


static mcz_cfg_t g_cfg = {0};

static bool inited = false;

mcz_cfg_t * mcz_config_get(void) {
    return &g_cfg;
}

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
static int parse_train_mode(const char *val, mcz_train_mode_t *out) {
    if (!val || !*val) { *out = MCZ_TRAIN_FAST; return 0; }
    if (!strcasecmp(val, "fast"))     { *out = MCZ_TRAIN_FAST;     return 0; }
    if (!strcasecmp(val, "optimize")) { *out = MCZ_TRAIN_OPTIMIZE; return 0; }

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

void mcz_init_default_config(void) {
    if(inited) return;
    g_cfg.enable_comp           = MCZ_DEFAULT_ENABLE_COMP;
    g_cfg.enable_dict           = MCZ_DEFAULT_ENABLE_DICT;
    g_cfg.dict_dir              = MCZ_DEFAULT_DICT_DIR;
    g_cfg.dict_size             = MCZ_DEFAULT_DICT_SIZE;
    g_cfg.zstd_level            = MCZ_DEFAULT_ZSTD_LEVEL;
    g_cfg.min_comp_size         = MCZ_DEFAULT_MIN_COMP_SIZE;
    g_cfg.max_comp_size         = MCZ_DEFAULT_MAX_COMP_SIZE;

    g_cfg.enable_training       = MCZ_DEFAULT_ENABLE_TRAINING;
    g_cfg.retraining_interval_s = MCZ_DEFAULT_RETRAIN_INTERVAL_S;
    g_cfg.min_training_size     = MCZ_DEFAULT_MIN_TRAINING_SIZE;
    g_cfg.ewma_alpha            = MCZ_DEFAULT_EWMA_ALPHA;
    g_cfg.retrain_drop          = MCZ_DEFAULT_RETRAIN_DROP;
    g_cfg.train_mode            = MCZ_DEFAULT_TRAIN_MODE;

    g_cfg.gc_cool_period        = MCZ_DEFAULT_GC_COOL_PERIOD;
    g_cfg.gc_quarantine_period  = MCZ_DEFAULT_GC_QUARANTINE_PERIOD;

    g_cfg.dict_retain_max       = MCZ_DEFAULT_DICT_RETAIN_MAX;

    g_cfg.enable_sampling       = MCZ_DEFAULT_ENABLE_SAMPLING;
    g_cfg.sample_p              = MCZ_DEFAULT_SAMPLE_P;
    g_cfg.sample_window_duration     = MCZ_DEFAULT_SAMPLE_WINDOW_DURATION;
    g_cfg.spool_dir             = MCZ_DEFAULT_SPOOL_DIR;
    g_cfg.spool_max_bytes       = MCZ_DEFAULT_SPOOL_MAX_BYTES;
    g_cfg.compress_keys         = MCZ_DEFAULT_COMPRESS_KEYS;
    inited = true;
}

static const char *train_mode_to_str(mcz_train_mode_t mode) {
    switch (mode) {
        case MCZ_TRAIN_FAST:     return "FAST";
        case MCZ_TRAIN_OPTIMIZE: return "OPTIMIZE";
        default:                 return "UNKNOWN";
    }
}

static inline bool strnull_or_empty(const char *s) {
    return (s == NULL) || (s[0] == '\0');
}

int mcz_config_sanity_check(void) {
    mcz_cfg_t *cfg = &g_cfg;
    if (!cfg) return -1;

    if (!cfg->enable_comp) return 0;
    bool ok = true;

    // dict_dir must exist
    if (strnull_or_empty(cfg->dict_dir)) {
        fprintf(stderr, "[mcz] sanity check: dict_dir is missing\n");
        ok = false;
    }

    // spool_dir required if sampling enabled
    if (cfg->enable_sampling && strnull_or_empty(cfg->spool_dir)) {
        fprintf(stderr, "[mcz] sanity check: sampling enabled but spool_dir is missing\n");
        ok = false;
    }

    if (!ok) {
        cfg->enable_dict = false;
        cfg->enable_training = false;
        fprintf(stderr, "[mcz] sanity check: dictionary compression is disabled\n");
        return -1;
    }

    return 0;
}


/*-------------------------------------------------------------------------*/
int parse_mcz_config(const char *path)
{
    mcz_init_default_config();
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
        if (strcasecmp(key, "mcz.level") == 0) {
            char *end; errno = 0; long lvl = strtol(val, &end, 10);
            if (end == val || *end || errno) { fprintf(stderr, "%s:%d: invalid level '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            if (lvl < 1 || lvl > 22) { fprintf(stderr, "%s:%d: level %ld out of range (1-22)\n", path, ln, lvl); rc = rc?rc:-ERANGE; continue; }
            g_cfg.zstd_level = (int)lvl;

        } else if (strcasecmp(key, "mcz.dict_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad dict size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.dict_size = (size_t)v;
            if (!strcasecmp(key,"max_dict")) fprintf(stderr, "%s:%d: NOTE: 'max_dict' is deprecated; use 'mcz.dict_size'\n", path, ln);

        } else if (strcasecmp(key, "mcz.min_training_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad min_training_size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.min_training_size = (size_t)v;
            if (!strcasecmp(key,"min_train_size")) fprintf(stderr, "%s:%d: NOTE: 'min_train_size' is deprecated; use 'mcz.min_training_size'\n", path, ln);

        } else if (strcasecmp(key, "mcz.min_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad min_size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.min_comp_size = (size_t)v;

        } else if (strcasecmp(key, "mcz.max_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad max_size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.max_comp_size = (size_t)v;

        } else if (strcasecmp(key, "mcz.dict_dir") == 0) {
            g_cfg.dict_dir = val && *val ? strdup(val) : NULL;
            if (!strcasecmp(key,"dict_dir_path")) fprintf(stderr, "%s:%d: NOTE: 'dict_dir_path' is deprecated; use 'mcz.dict_dir'\n", path, ln);

        } else if (strcasecmp(key, "mcz.enable_dict") == 0) {
            bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.enable_dict = b;
        } else if (strcasecmp(key, "mcz.enable_comp") == 0) {
            bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.enable_comp = b;
        } else if (strcasecmp(key, "mcz.enable_training") == 0) {
            bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.enable_training = b;
        } else if (strcasecmp(key, "mcz.retraining_interval") == 0) {
            int64_t s; if (parse_duration_sec(val, &s)) { fprintf(stderr, "%s:%d: bad retraining_interval '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.retraining_interval_s = s;
        } else if (strcasecmp(key, "mcz.min_training_size") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad min_training_size '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.min_training_size = (size_t)v;
        } else if (strcasecmp(key, "mcz.ewma_alpha") == 0) {
            double d; if (parse_frac(val, &d)) { fprintf(stderr, "%s:%d: bad ewma_alpha '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.ewma_alpha = d;
        } else if (strcasecmp(key, "mcz.retrain_drop") == 0) {
            double d; if (parse_frac(val, &d)) { fprintf(stderr, "%s:%d: bad retrain_drop '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.retrain_drop = d;
        } else if (!strcasecmp(key, "mcz.train_mode")) {
            mcz_train_mode_t mode;
            if(parse_train_mode(val, &mode)) { fprintf(stderr, "%s:%d: bad train_mode'%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue;}
            g_cfg.train_mode = mode;
        } else if (strcasecmp(key, "mcz.gc_cool_period") == 0) {
            int64_t s; if (parse_duration_sec(val, &s)) { fprintf(stderr, "%s:%d: bad gc_cool_period '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.gc_cool_period = s;
        } else if (strcasecmp(key, "mcz.gc_quarantine_period") == 0) {
               int64_t s; if (parse_duration_sec(val, &s)) { fprintf(stderr, "%s:%d: bad gc_quarantine_period '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.gc_quarantine_period = s;
        /* Retention */
        } else if (strcasecmp(key, "mcz.dict_retain_max") == 0) {
            char *end; long v = strtol(val, &end, 10);
            if (val == end || *end || v < 1 || v > 256) { fprintf(stderr, "%s:%d: bad dict_retain_max '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.dict_retain_max = (int)v;

            /* Sampling + Spool */
        } else if (strcasecmp(key, "mcz.enable_sampling") == 0) {
            bool b; if (parse_bool(val, &b)) { fprintf(stderr, "%s:%d: bad bool '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.enable_sampling = b;
        } else if (strcasecmp(key, "mcz.sample_p") == 0) {
            double d; if (parse_frac(val, &d)) { fprintf(stderr, "%s:%d: bad sample_p '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.sample_p = d;
        } else if (strcasecmp(key, "mcz.sample_window_duration") == 0) {
            int64_t s; if (parse_duration_sec(val, &s)) { fprintf(stderr, "%s:%d: bad sample_window_duration '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.sample_window_duration = s;
        } else if (strcasecmp(key, "mcz.spool_dir") == 0) {
            g_cfg.spool_dir = val && *val ? strdup(val) : NULL;
        } else if (strcasecmp(key, "mcz.spool_max_bytes") == 0) {
            int64_t v; if (parse_bytes(val, &v)) { fprintf(stderr, "%s:%d: bad spool_max_bytes '%s'\n", path, ln, val); rc = rc?rc:-EINVAL; continue; }
            g_cfg.spool_max_bytes = (size_t)v;
        } else if (strcasecmp(key, "compress_keys") == 0) {
            /* legacy: ignored in MCZ; accept to avoid breaking configs */
            fprintf(stderr, "%s:%d: NOTE: 'compress_keys' ignored\n", path, ln);
        } else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, ln, key);
            /* not fatal; continue */
        }
    }
    free(line);
    fclose(fp);
    /* basic sanity checks */
    if (g_cfg.min_comp_size > g_cfg.max_comp_size) {
        fprintf(stderr, "mcz: min_size > max_size\n"); rc = rc?rc:-EINVAL; goto err;
    }
    if (g_cfg.enable_sampling && (g_cfg.sample_p <= 0.0 || g_cfg.sample_p > 1.0)) {
        fprintf(stderr, "mcz: sample_p must be in (0,1]\n"); rc = rc?rc:-ERANGE; goto err;
    }
    if (g_cfg.dict_dir == NULL && g_cfg.enable_comp && g_cfg.enable_dict){
        fprintf(stderr, "mcz: dictionary directory is not specified\n"); rc = rc?rc:-EINVAL; goto err;
    }
    if (g_cfg.spool_dir == NULL && g_cfg.enable_comp && g_cfg.enable_dict){
        fprintf(stderr, "mcz: spoll directory is not specified\n"); rc = rc?rc:-EINVAL; goto err;
    }

    return rc;      /* 0 if perfect, first fatal errno otherwise */
err: // set compression to disabled
    fprintf(stderr, "mcz: compression disabled due to an error in the configuration file\n");
    g_cfg.enable_comp = false;
    g_cfg.enable_dict = false;
    return rc;
}

void mcz_config_print(const mcz_cfg_t *cfg) {
    if (!cfg) {
        printf("(null config)\n");
        return;
    }

    printf("=== MCZ Configuration ===\n");

    // Core
    printf("enable_comp        : %s\n", cfg->enable_comp ? "true" : "false");
    printf("enable_dict        : %s\n", cfg->enable_dict ? "true" : "false");
    printf("dict_dir           : %s\n", cfg->dict_dir ? cfg->dict_dir : "(null)");
    printf("dict_size          : %zu\n", cfg->dict_size);
    printf("zstd_level         : %d\n", cfg->zstd_level);
    printf("min_comp_size      : %zu\n", cfg->min_comp_size);
    printf("max_comp_size      : %zu\n", cfg->max_comp_size);
    printf("compress_keys      : %s\n", cfg->compress_keys ? "true" : "false");

    // Training
    printf("enable_training         : %s\n", cfg->enable_training ? "true" : "false");
    printf("retraining_interval_s   : %" PRId64 "\n", cfg->retraining_interval_s);
    printf("min_training_size       : %zu\n", cfg->min_training_size);
    printf("ewma_alpha              : %.3f\n", cfg->ewma_alpha);
    printf("retrain_drop            : %.3f\n", cfg->retrain_drop);
    printf("train_mode              : %s\n", train_mode_to_str(cfg->train_mode));

    // GC
    printf("gc_cool_period          : %d\n", cfg->gc_cool_period);
    printf("gc_quarantine_period    : %d\n", cfg->gc_quarantine_period);

    // Retention
    printf("dict_retain_max         : %d\n", cfg->dict_retain_max);

    // Sampling + Spool
    printf("enable_sampling         : %s\n", cfg->enable_sampling ? "true" : "false");
    printf("sample_p                : %.3f\n", cfg->sample_p);
    printf("sample_window_duration  : %d\n", cfg->sample_window_duration);
    printf("spool_dir               : %s\n", cfg->spool_dir ? cfg->spool_dir : "(null)");
    printf("spool_max_bytes         : %zu\n", cfg->spool_max_bytes);

    printf("=========================\n");
}

