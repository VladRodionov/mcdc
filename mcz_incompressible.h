// mcz_incompressible.h  — fast compressibility check for raw blobs
// Compile: cc -O3 -std=c11 your.c -lm            (no probe)
//          cc -O3 -std=c11 your.c -lm -lzstd -DMCZ_ENABLE_ZSTD_PROBE   (with probe)
// You can override defaults with -DMCZ_SAMPLE_BYTES=512 etc.

#ifndef MCZ_INCOMPRESSIBLE_H
#define MCZ_INCOMPRESSIBLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifndef MCZ_MIN_COMP_SIZE
#define MCZ_MIN_COMP_SIZE      32u      // don't bother below this
#endif

#ifndef MCZ_SAMPLE_BYTES
#define MCZ_SAMPLE_BYTES       512u     // ~500 bytes is enough; bounded stack probe
#endif

#ifndef MCZ_ASCII_THRESHOLD
#define MCZ_ASCII_THRESHOLD    0.85     // ≥85% printable ASCII -> compress
#endif

#ifndef MCZ_ENTROPY_NO
#define MCZ_ENTROPY_NO         7.50     // H8 ≥ 7.5 bits/byte -> skip
#endif

#ifndef MCZ_ENTROPY_YES
#define MCZ_ENTROPY_YES        7.00     // H8 ≤ 7.0 bits/byte -> compress
#endif

#ifndef MCZ_PROBE_MIN_GAIN
#define MCZ_PROBE_MIN_GAIN     0.02     // ≥2% savings on sample -> compress
#endif

// Safe conservative upper bound: src + src/128 + 256
enum { MCZ_PROBE_DSTMAX = (int)(MCZ_SAMPLE_BYTES + (MCZ_SAMPLE_BYTES >> 7) + 256) };

bool is_likely_incompressible(const uint8_t *p, size_t n);

#endif
