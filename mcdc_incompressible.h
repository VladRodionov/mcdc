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
 * mcdc_incompressible.c
 *
 * Implementation of incompressible payload detection for MC/DC.
 *
 * Key duties:
 *   - Evaluate data samples for entropy or patterns.
 *   - Quickly decide whether to bypass compression.
 *   - Maintain state for tuning thresholds if enabled.
 *
 *   You can override defaults with -DMCDC_SAMPLE_BYTES=512 etc.
 */
#ifndef MCDC_INCOMPRESSIBLE_H
#define MCDC_INCOMPRESSIBLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MCDC_SAMPLE_BYTES
#define MCDC_SAMPLE_BYTES       512u     // ~500 bytes is enough; bounded stack probe
#endif

#ifndef MCDC_ASCII_THRESHOLD
#define MCDC_ASCII_THRESHOLD    0.85     // ≥85% printable ASCII -> compress
#endif

#ifndef MCDC_ENTROPY_NO
#define MCDC_ENTROPY_NO         7.50     // H8 ≥ 7.5 bits/byte -> skip
#endif

#ifndef MCDC_ENTROPY_YES
#define MCDC_ENTROPY_YES        7.00     // H8 ≤ 7.0 bits/byte -> compress
#endif

#ifndef MCDC_PROBE_MIN_GAIN
#define MCDC_PROBE_MIN_GAIN     0.02     // ≥2% savings on sample -> compress
#endif

// Safe conservative upper bound: src + src/128 + 256
enum { MCDC_PROBE_DSTMAX = (int)(MCDC_SAMPLE_BYTES + (MCDC_SAMPLE_BYTES >> 7) + 256) };

bool is_likely_incompressible(const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif
#endif
