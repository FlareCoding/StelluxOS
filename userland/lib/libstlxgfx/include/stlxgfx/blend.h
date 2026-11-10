#ifndef STLXGFX_BLEND_H
#define STLXGFX_BLEND_H

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

/* The alpha of a premultiplied pixel of alpha `below` after a
 * source-over at `alpha`, truncated as `stlxgfx_blend_channel`
 * truncates the color, so no channel exceeds it */
static inline uint8_t stlxgfx_blend_alpha(uint32_t below, uint32_t alpha) {
    return (uint8_t)(alpha + stlxgfx_div255(below * (255 - alpha)));
}

/* Source-over of `src` onto `dst` in straight alpha, where color is
 * not multiplied by alpha. Both are ARGB, and `dst` alpha must be
 * below 255, since opaque pixels blend by `stlxgfx_blend_channel`.
 * Onto alpha 0 the result is `src`. Otherwise the alpha is the
 * coverage of both pixels together, rounded up, and each channel the
 * mean of both colors weighted by the coverage each shows, rounded,
 * so it lies between them. */
static inline uint32_t stlxgfx_blend_straight(uint32_t dst, uint32_t src) {
    uint32_t src_alpha = src >> 24;
    uint32_t dst_alpha = dst >> 24;
    if (dst_alpha == 0) {
        return src;
    }

    uint32_t src_weight = src_alpha * 255;
    uint32_t dst_weight = dst_alpha * (255 - src_alpha);
    uint32_t weight = src_weight + dst_weight;
    uint32_t blended = (src_alpha + (dst_weight + 254) / 255) << 24;

    for (uint32_t shift = 0; shift < 24; shift += 8) {
        uint32_t src_channel = (src >> shift) & 0xFF;
        uint32_t dst_channel = (dst >> shift) & 0xFF;
        uint32_t sum = src_channel * src_weight + dst_channel * dst_weight;
        blended |= ((sum + weight / 2) / weight) << shift;
    }

    return blended;
}

#endif /* STLXGFX_BLEND_H */
