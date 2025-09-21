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
 * mcz_incompressible.c
 *
 * Fast detection of incompressible payloads in memcached-Zstd (MCZ).
 *
 * Responsibilities:
 *   - Maintain heuristics for early rejection of incompressible data.
 *   - Provide APIs to check whether a sample should be skipped.
 *   - Keep simple statistics to refine heuristics (optional).
 *
 * Design:
 *   - Header-only declarations; lightweight hot-path functions.
 *   - Integrated with compression pipeline before dictionary/level selection.
 *   - All functions prefixed with `mcz_` for clarity.
 */
#include "mcz_incompressible.h"
#include <zstd.h>

// Forward definitions
bool mcz_has_prefix(const uint8_t *p, size_t n, const char *sig, size_t m);
bool mcz_looks_like_compressed_or_media(const uint8_t *p, size_t n);
double mcz_ascii_ratio_sample(const uint8_t *p, size_t n);
double mcz_entropy_h8_sample(const uint8_t *p, size_t n);
bool mcz_probe_zstd_l1_saves(const uint8_t *p, size_t n) ;
bool mcz_looks_like_base64(const uint8_t *p, size_t n);

// ---- helpers ---------------------------------------------------------------

inline bool mcz_has_prefix(const uint8_t *p, size_t n, const char *sig, size_t m) {
    return n >= m && memcmp(p, sig, m) == 0;
}

inline bool mcz_looks_like_compressed_or_media(const uint8_t *p, size_t n) {
    // Already-compressed containers/codecs
    if (mcz_has_prefix(p,n,"\x28\xB5\x2F\xFD",4)) return true;       // zstd
    if (mcz_has_prefix(p,n,"\x1F\x8B",2))        return true;        // gzip
    if (n>=2){ unsigned cmf=p[0], flg=p[1]; if ((cmf&0x0F)==8 && ((cmf<<8)+flg)%31==0) return true; } // zlib
    if (mcz_has_prefix(p,n,"\x04\x22\x4D\x18",4) ||
        mcz_has_prefix(p,n,"\x02\x21\x4C\x18",4)) return true;       // lz4 (modern/legacy)
    if (mcz_has_prefix(p,n,"\x50\x4B\x03\x04",4)) return true;       // zip
    if (mcz_has_prefix(p,n,"\xFD\x37\x7A\x58\x5A\x00",6)) return true;// xz
    if (mcz_has_prefix(p,n,"BZh",3))              return true;        // bzip2
    // Common opaque media
    if (mcz_has_prefix(p,n,"\xFF\xD8",2))         return true;        // jpeg
    if (mcz_has_prefix(p,n,"\x89PNG\r\n\x1A\n",8))return true;        // png
    if (mcz_has_prefix(p,n,"GIF87a",6) || mcz_has_prefix(p,n,"GIF89a",6)) return true; // gif
    if (mcz_has_prefix(p,n,"OggS",4))             return true;        // ogg
    if (n>=12 && mcz_has_prefix(p,n,"RIFF",4) && memcmp(p+8,"WEBP",4)==0) return true; // webp
    if (n>=8  && memcmp(p+4,"ftyp",4)==0)         return true;        // mp4/iso-bmff
    if (mcz_has_prefix(p,n,"ID3",3))              return true;        // mp3 id3
    if (mcz_has_prefix(p,n,"%PDF-",5))            return true;        // pdf
    return false;
}

inline double mcz_ascii_ratio_sample(const uint8_t *p, size_t n) {
    size_t S = n < MCZ_SAMPLE_BYTES ? n : MCZ_SAMPLE_BYTES;
    if (S == 0) return 0.0;
    size_t ascii = 0;
    for (size_t i = 0; i < S; i++) {
        uint8_t c = p[i];
        ascii += (c == 9) | (c == 10) | (c == 13) | (c >= 32 && c <= 126);
    }
    return (double)ascii / (double)S;
}

inline double mcz_entropy_h8_sample(const uint8_t *p, size_t n) {
    size_t S = n < MCZ_SAMPLE_BYTES ? n : MCZ_SAMPLE_BYTES;
    if (S == 0) return 8.0;
    uint16_t h[256] = {0};  // 512B sample fits in 16-bit bins
    for (size_t i = 0; i < S; i++) h[p[i]]++;
    double H = 0.0, invlog2 = 1.0 / log(2.0);
    for (int b = 0; b < 256; b++) if (h[b]) {
        double pb = (double)h[b] / (double)S;
        H -= pb * (log(pb) * invlog2);
    }
    return H; // 0..8 bits/byte
}

inline bool mcz_probe_zstd_l1_saves(const uint8_t *p, size_t n) {
    size_t S = n < MCZ_SAMPLE_BYTES ? n : MCZ_SAMPLE_BYTES;
    if (S == 0) return false;
    uint8_t dst[MCZ_PROBE_DSTMAX]; // fixed stack buffer
    size_t cs = ZSTD_compress(dst, sizeof(dst), p, S, 1); // level 1
    if (ZSTD_isError(cs)) return false;
    double gain = 1.0 - ((double)cs / (double)S);
    return gain >= MCZ_PROBE_MIN_GAIN;
}

inline bool mcz_looks_like_base64(const uint8_t *p, size_t n) {
    size_t S = n < MCZ_SAMPLE_BYTES ? n : MCZ_SAMPLE_BYTES;
    if (S < 128) return false;
    size_t ok = 0, eq = 0;
    for (size_t i=0;i<S;i++){
        uint8_t c=p[i];
        bool b64 = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'||c=='=';
        if (b64) ok++;
        if (c=='=') eq++;
    }
    return ((double)ok/(double)S) >= 0.90 && (eq <= S/4);
}

// ---- main decision ---------------------------------------------------------

// Return true  -> likely INCOMPRESSIBLE (skip)
//        false -> likely compressible (try compress)
// Pass your max_comp_size; if none, use SIZE_MAX.
inline bool is_likely_incompressible(const uint8_t *p, size_t n) {

    // 1) magic sniff
    if (mcz_looks_like_compressed_or_media(p, n)) return true;

    // 2) obvious text?
    if (mcz_ascii_ratio_sample(p, n) >= MCZ_ASCII_THRESHOLD) return false;

    // 3) entropy check on ~500B
    double H = mcz_entropy_h8_sample(p, n);
    if (H >= MCZ_ENTROPY_NO)  return true;
    if (H <= MCZ_ENTROPY_YES) return false;

    // 4) base64-ish blobs (often already-compressed data wrapped in text)
    if (mcz_looks_like_base64(p, n)) return true;

    // 5) optional micro-probe for ambiguous cases
    if (mcz_probe_zstd_l1_saves(p, n)) return false;

    // default on ambiguity: skip
    return true;
}
