/*-------------------------------- zstd_config.c -------------------------*/
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memcached.h"                  /* settings, zstd_cfg_t          */
#include "zstd_config.h"

static void ltrim(char **s)
{
    while (isspace((unsigned char)**s)) (*s)++;
}

static void rtrim(char *s)
{
    for (char *p = s + strlen(s) - 1; p >= s && isspace((unsigned char)*p); p--)
        *p = '\0';
}

/* K / M / G suffix → bytes ------------------------------------------------*/
static int64_t parse_bytes(const char *val, int64_t *out)
{
    char *end;
    double v = strtod(val, &end);
    if (val == end) return -EINVAL;             /* no digits */

    switch (toupper((unsigned char)*end)) {
        case 'G': v *= 1024;
        case 'M': v *= 1024;
        case 'K': v *= 1024;
        case '\0': break;
        default:  return -EINVAL;
    }
    if (v < 0 || v > INT64_MAX) return -ERANGE;
    *out = (int64_t)v;
    return 0;
}

#ifdef USE_ZSTD
/*-------------------------------------------------------------------------*/
int parse_zstd_config(const char *path)
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
        if (strcasecmp(key, "level") == 0) {
            char *end;
            long lvl = strtol(val, &end, 10);
            errno = 0;
            if (end == val || *end || errno > 0) {
                fprintf(stderr, "%s:%d: invalid level '%s'\n", path, ln, val);
                rc = rc ? rc: -EINVAL;  continue;
            }
            if (lvl < 1 || lvl > 22) {
                fprintf(stderr, "%s:%d: level %ld out of range (1-22)\n",
                        path, ln, lvl);
                rc = rc ? rc: -ERANGE;  continue;
            }
            settings.zstd_level = (int)lvl;

        } else if (strcasecmp(key, "max_dict") == 0) {
            int64_t v;
            if (parse_bytes(val, &v) != 0) {
                fprintf(stderr, "%s:%d: bad max_dict '%s'\n", path, ln, val);
                rc = rc ? rc: -EINVAL;  continue;
            }
            settings.zstd_max_dict = (size_t)v;

        } else if (strcasecmp(key, "min_train_size") == 0) {
            int64_t v;
            if (parse_bytes(val, &v) != 0) {
                fprintf(stderr, "%s:%d: bad min_train_size '%s'\n", path, ln, val);
                rc = rc ? rc: -EINVAL;  continue;
            }
            settings.zstd_min_train = (size_t)v;

        } else if (strcasecmp(key, "min_comp_size") == 0) {
            int64_t v;
            if (parse_bytes(val, &v) != 0) {
                fprintf(stderr, "%s:%d: bad min_comp_size '%s'\n", path, ln, val);
                rc = rc ? rc: -EINVAL;  continue;
            }
            settings.zstd_min_comp = (size_t)v;

        } else if (strcasecmp(key, "max_comp_size") == 0) {
            int64_t v;
            if (parse_bytes(val, &v) != 0) {
                fprintf(stderr, "%s:%d: bad max_comp_size '%s'\n", path, ln, val);
                rc = rc ? rc: -EINVAL;  continue;
            }
            settings.zstd_max_comp = (size_t)v;

        } else if (strcasecmp(key, "compress_keys") == 0) {
            settings.zstd_compress_keys = (strcasecmp(val, "true") == 0 ||
                                  strcmp(val, "1") == 0 ||
                                  strcasecmp(val, "yes") == 0);

        } else if (strcasecmp(key, "dict_dir_path") == 0) {
            settings.zstd_dict_dir = val? strdup(val): NULL;
        } else if (strcasecmp(key, "disable_dict") == 0) {
                settings.disable_dict = (strcasecmp(val, "true") == 0 ||
                                          strcmp(val, "1") == 0 ||
                                          strcasecmp(val, "yes") == 0);
        } else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, ln, key);
            /* not fatal, keep parsing */
        }
    }

    free(line);
    fclose(fp);
    return rc;      /* 0 if perfect, first fatal errno otherwise */
}
#endif
