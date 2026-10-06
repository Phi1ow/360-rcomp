/* Original R-comp depth/stencil probe. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RCOMP_DEPTH_ORACLE_H
#define RCOMP_DEPTH_ORACLE_H
#include <stdint.h>
#include <string.h>
#define DP_W 1280u
#define DP_SW 640u
#define DP_H 1024u
/* Exactly representable binary fractions; no F24 conversion on this route. */
static inline float dp_pattern_depth(uint32_t x,uint32_t y,uint32_t sample) {
    return 0.125f + (float)((x*3u+y*5u+sample*7u)&7u)*0.0625f;
}
static inline uint32_t dp_pattern_stencil(uint32_t x,uint32_t y,uint32_t sample) {
    return (x*13u+y*7u+sample*29u)&255u;
}
static inline uint32_t dp_source_sample(uint32_t x,uint32_t sample) {
    return (x&1u)|((sample^1u)<<1u);
}
static inline uint32_t dp_float_bits(float value) {
    uint32_t bits; memcpy(&bits,&value,sizeof(bits)); return bits;
}
static inline uint32_t dp_color(uint32_t tile) {
    return 0xFF000000u | (0xFFu<<(tile*8u));
}
#endif
