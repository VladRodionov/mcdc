/*----------------------------------------------------------------------
 * zstd_config.h
 *
 * Simple INI-style configuration loader for Zstandard tuning parameters.
 * Called from option parsing:  parse_zstd_config(<path>, &g_zstd.cfg)
 *
 * Copyright © 2025  <Your Name/Company>
 *--------------------------------------------------------------------*/
#ifndef ZSTD_CONFIG
#define ZSTD_CONFIG

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------------
 * User-tunable parameters for the zstd integration
 * (keep in sync with zstd_compression.h)
 * ------------------------------------------------------------------ */
typedef struct {
    int      level;            /* compression level (1-22), 0 = default  */
    size_t   max_dict;         /* maximum dictionary size (bytes)        */
    size_t   min_train_size;   /* corpus size before training (bytes)    */
    size_t   min_comp_size;    /* skip compression below this size       */
    size_t   max_comp_size;    /* skip compression above this size       */
    bool     compress_keys;    /* true => compress keys as well          */
    const char *dict_dir_path; /* NULL ⇒ live training; else preload dir */
    bool     disable_dict;
    bool     disable_comp;
} zstd_cfg_t;

/* --------------------------------------------------------------------
 * parse_zstd_config()
 *
 *  - Reads an INI-style file (key = value, “#” comments).
 *  - Recognised keys: level, max_dict, min_train_size,
 *    min_comp_size, max_comp_size, compress_keys, dict_dir_path.
 *  - Size values accept K/M/G suffixes (case-insensitive).
 *  - Populates / overrides fields in *cfg.
 *
 * Returns:
 *      0          on success
 *     -errno      on I/O failure (-ENOENT, -EACCES, …)
 *     -EINVAL     on syntax error
 *-------------------------------------------------------------------*/
int parse_zstd_config(const char *path);

#endif /* ZSTD_CONFIG_H */
