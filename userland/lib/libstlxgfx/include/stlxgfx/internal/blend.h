#ifndef STLXGFX_INTERNAL_BLEND_H
#define STLXGFX_INTERNAL_BLEND_H

#include <stdint.h>

/* Exact truncating x/255 for x in [0, 65025]. Bit-identical to the
 * divide it replaces, unlike the usual approximations. */
static inline uint32_t stlxgfx_div255(uint32_t x) {
    return (x + 1 + (x >> 8)) >> 8;
}

/* Source-over for one channel. src_premul is src * alpha so callers
 * with a constant color hoist the multiply out of their pixel loop. */
static inline uint8_t stlxgfx_blend_channel(uint8_t dst, uint32_t src_premul,
                                            uint32_t inv_alpha) {
    return (uint8_t)stlxgfx_div255(src_premul + dst * inv_alpha);
}

#endif /* STLXGFX_INTERNAL_BLEND_H */
