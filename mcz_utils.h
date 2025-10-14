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
 * mcz_utils.h
 *
 * Implementation of shared utility functions used by MCZ modules.
 *
 * Key duties:
 *   - Error string formatting.
 *   - Atomic file/text writes.
 *   - Time/date formatting in RFC3339 UTC.
 *   - Safe directory sync after file operations.
 *   - String utilities (e.g., join namespace array).
 */
#ifndef MCZ_UTILS_H
#define MCZ_UTILS_H
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#include <ctype.h>

#ifdef __cplusplus
extern "C" {
#endif
/*
 * Common utility functions for MCZ project.
 *
 * Caller is responsible for freeing strings allocated by
 * set_err() with free().
 */

void sleep_ms(unsigned ms);

void set_err(char **err_out, const char *fmt, ...);

void format_rfc3339_utc(time_t t, char out[32]);

int atomic_write_file(const char *dir, const char *final_path,
                      const void *data, size_t len, mode_t mode,
                      char **err_out);

int atomic_write_text(const char *dir, const char *final_path,
                      const char *text, char **err_out);
int fsync_dirpath(const char *dirpath);

char *mcz_join_namespaces(const char * const *prefixes, size_t nprefixes,
                          const char *sep);

uint64_t now_usec(void);

void log_rate_limited(uint64_t interval_us, const char *fmt, ...);

int str_to_u16(const char *s, uint16_t *out);

char *xstrdup(const char *s);

void trim(char *s);

int join_path(char *dst, size_t cap, const char *dir, const char *file);

int parse_rfc3339_utc(const char *s, time_t *out);

int split_prefixes(char *csv, char ***out, size_t *nout);

int uuidv4_string(char out[37]);

int make_uuid_basename(const char *ext, char out[64], char **err_out);

uint32_t fast_rand32(void);

uint64_t fnv1a64(const char *s);

void *xzmalloc(size_t n);

/* --------- 32-bit helpers --------- */

/* Read an _Atomic uint32_t */
uint32_t
atomic_get32(const _Atomic uint32_t *p);

/* Set an _Atomic uint32_t */
void
atomic_set32(_Atomic uint32_t *p, uint32_t v);

/* Increment an _Atomic uint32_t by delta, return the new value */
uint32_t
atomic_inc32(_Atomic uint32_t *p, uint32_t delta);
/* --------- 64-bit helpers --------- */

/* Read an _Atomic uint64_t */
uint64_t
atomic_get64(const _Atomic uint64_t *p);

/* Set an _Atomic uint64_t */
void
atomic_set64(_Atomic uint64_t *p, uint64_t v);

/* Increment an _Atomic uint64_t by delta, return the new value */
uint64_t
atomic_inc64(_Atomic uint64_t *p, uint64_t delta);

/* Read an _Atomic int64_t */
int64_t
atomic_get64s(const _Atomic int64_t *p);

/* Set an _Atomic int64_t */
void
atomic_set64s(_Atomic int64_t *p, int64_t v);

/* Increment an _Atomic int64_t by delta, return the new value */
int64_t
atomic_inc64s(_Atomic int64_t *p, int64_t delta);

#ifdef __cplusplus
}
#endif
#endif /* MCZ_UTILS_H */
