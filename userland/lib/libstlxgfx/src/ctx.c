#include <stlxgfx/ctx.h>
#include <stlxgfx/font.h>
#include <math.h>
#include <string.h>

void stlxgfx_ctx_init(stlxgfx_ctx_t *ctx, stlxgfx_surface_t *target) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->target = target;
    ctx->state.ox = 0;
    ctx->state.oy = 0;
    ctx->state.clip.x = 0;
    ctx->state.clip.y = 0;
    ctx->state.clip.w = target ? target->width : 0;
    ctx->state.clip.h = target ? target->height : 0;
    ctx->stack_depth = 0;
}

int stlxgfx_ctx_save(stlxgfx_ctx_t *ctx) {
    if (ctx->stack_depth >= STLXGFX_CTX_MAX_SAVE_DEPTH) {
        return -1;
    }

    ctx->stack[ctx->stack_depth++] = ctx->state;
    return 0;
}

int stlxgfx_ctx_restore(stlxgfx_ctx_t *ctx) {
    if (ctx->stack_depth <= 0) {
        return -1;
    }

    ctx->state = ctx->stack[--ctx->stack_depth];
    return 0;
}

void stlxgfx_ctx_translate(stlxgfx_ctx_t *ctx, int32_t dx, int32_t dy) {
    ctx->state.ox += dx;
    ctx->state.oy += dy;
}

void stlxgfx_ctx_clip(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                       uint32_t w, uint32_t h) {
    int32_t ax = ctx->state.ox + x;
    int32_t ay = ctx->state.oy + y;
    int32_t ax1 = ax + (int32_t)w;
    int32_t ay1 = ay + (int32_t)h;

    stlxgfx_clip_t *c = &ctx->state.clip;
    int32_t cx1 = c->x + (int32_t)c->w;
    int32_t cy1 = c->y + (int32_t)c->h;

    int32_t nx0 = ax > c->x ? ax : c->x;
    int32_t ny0 = ay > c->y ? ay : c->y;
    int32_t nx1 = ax1 < cx1 ? ax1 : cx1;
    int32_t ny1 = ay1 < cy1 ? ay1 : cy1;

    if (nx0 >= nx1 || ny0 >= ny1) {
        c->x = 0;
        c->y = 0;
        c->w = 0;
        c->h = 0;
    } else {
        c->x = nx0;
        c->y = ny0;
        c->w = (uint32_t)(nx1 - nx0);
        c->h = (uint32_t)(ny1 - ny0);
    }
}

void stlxgfx_ctx_reset_clip(stlxgfx_ctx_t *ctx) {
    ctx->state.clip.x = 0;
    ctx->state.clip.y = 0;
    ctx->state.clip.w = ctx->target ? ctx->target->width : 0;
    ctx->state.clip.h = ctx->target ? ctx->target->height : 0;
}

static int ctx_clip_rect(const stlxgfx_ctx_t *ctx,
                          int32_t x, int32_t y, uint32_t w, uint32_t h,
                          int32_t *ox, int32_t *oy, uint32_t *ow, uint32_t *oh) {
    int32_t ax = ctx->state.ox + x;
    int32_t ay = ctx->state.oy + y;
    int32_t ax1 = ax + (int32_t)w;
    int32_t ay1 = ay + (int32_t)h;

    const stlxgfx_clip_t *c = &ctx->state.clip;
    int32_t cx1 = c->x + (int32_t)c->w;
    int32_t cy1 = c->y + (int32_t)c->h;

    int32_t rx0 = ax > c->x ? ax : c->x;
    int32_t ry0 = ay > c->y ? ay : c->y;
    int32_t rx1 = ax1 < cx1 ? ax1 : cx1;
    int32_t ry1 = ay1 < cy1 ? ay1 : cy1;

    if (rx0 >= rx1 || ry0 >= ry1) {
        return 0;
    }

    *ox = rx0;
    *oy = ry0;
    *ow = (uint32_t)(rx1 - rx0);
    *oh = (uint32_t)(ry1 - ry0);
    return 1;
}

void stlxgfx_ctx_clear(stlxgfx_ctx_t *ctx, uint32_t color) {
    if (!ctx || !ctx->target) {
        return;
    }

    const stlxgfx_clip_t *c = &ctx->state.clip;
    if (c->w == 0 || c->h == 0) {
        return;
    }

    stlxgfx_fill_rect(ctx->target, c->x, c->y, c->w, c->h, color);
}

void stlxgfx_ctx_fill_rect(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t color) {
    if (!ctx || !ctx->target) {
        return;
    }

    int32_t ox, oy;
    uint32_t ow, oh;
    if (!ctx_clip_rect(ctx, x, y, w, h, &ox, &oy, &ow, &oh)) {
        return;
    }

    // A translucent color composites over the target, an opaque one
    // keeps the raw fast path
    if (((color >> 24) & 0xFF) == 0xFF) {
        stlxgfx_fill_rect(ctx->target, ox, oy, ow, oh, color);
    } else {
        stlxgfx_fill_rect_blend(ctx->target, ox, oy, ow, oh, color);
    }
}

/* Edge coverage for a pixel at distance dist from an arc of radius r:
 * full inside, zero outside, one linear pixel across the boundary. */
static uint8_t arc_coverage(float dist, float radius) {
    float c = radius + 0.5f - dist;
    if (c <= 0.0f) {
        return 0;
    }

    if (c >= 1.0f) {
        return 255;
    }

    return (uint8_t)(c * 255.0f + 0.5f);
}

static int ctx_clip_contains(const stlxgfx_ctx_t *ctx, int32_t x, int32_t y) {
    const stlxgfx_clip_t *c = &ctx->state.clip;
    return x >= c->x && x < c->x + (int32_t)c->w &&
           y >= c->y && y < c->y + (int32_t)c->h;
}

void stlxgfx_ctx_fill_arc_corner(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                                  uint32_t r_outer, uint32_t r_inner,
                                  int dir_x, int dir_y, int invert,
                                  uint32_t color) {
    if (!ctx || !ctx->target || r_outer == 0) {
        return;
    }

    int32_t r = (int32_t)r_outer;
    float corner_x = (float)(ctx->state.ox + x);
    float corner_y = (float)(ctx->state.oy + y);
    float center_x = corner_x + (float)(dir_x * r);
    float center_y = corner_y + (float)(dir_y * r);

    for (int32_t oy = 0; oy < r; oy++) {
        for (int32_t ox = 0; ox < r; ox++) {
            int32_t px = (int32_t)corner_x + (dir_x > 0 ? ox : -1 - ox);
            int32_t py = (int32_t)corner_y + (dir_y > 0 ? oy : -1 - oy);
            if (!ctx_clip_contains(ctx, px, py)) {
                continue;
            }

            float dx = ((float)px + 0.5f) - center_x;
            float dy = ((float)py + 0.5f) - center_y;
            float dist = sqrtf(dx * dx + dy * dy);
            uint8_t cov_out = arc_coverage(dist, (float)r);
            uint8_t cov;
            if (invert) {
                cov = (uint8_t)(255 - cov_out);
            } else if (r_inner > 0) {
                uint8_t cov_in = arc_coverage(dist, (float)r_inner);
                cov = cov_out > cov_in ? (uint8_t)(cov_out - cov_in) : 0;
            } else {
                cov = cov_out;
            }

            if (cov) {
                stlxgfx_blend_coverage(ctx->target, px, py, color, cov);
            }
        }
    }
}

void stlxgfx_ctx_draw_rect(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t color) {
    if (!ctx || !ctx->target || w == 0 || h == 0) {
        return;
    }

    stlxgfx_ctx_fill_rect(ctx, x, y, w, 1, color);
    if (h > 1) {
        stlxgfx_ctx_fill_rect(ctx, x, y + (int32_t)h - 1, w, 1, color);
    }
    if (h > 2) {
        stlxgfx_ctx_fill_rect(ctx, x, y + 1, 1, h - 2, color);
        if (w > 1) {
            stlxgfx_ctx_fill_rect(ctx, x + (int32_t)w - 1, y + 1, 1, h - 2, color);
        }
    }
}

void stlxgfx_ctx_fill_circle(stlxgfx_ctx_t *ctx, int32_t cx, int32_t cy,
                              uint32_t radius, uint32_t color) {
    if (!ctx || !ctx->target || radius == 0) {
        return;
    }

    // Continuous center on the middle of pixel (cx, cy), edge coverage
    // puts the rim at radius + 0.5 so the diameter stays 2r + 1 pixels
    float fcx = (float)(ctx->state.ox + cx) + 0.5f;
    float fcy = (float)(ctx->state.oy + cy) + 0.5f;
    int32_t r = (int32_t)radius;
    int32_t x0 = ctx->state.ox + cx - r;
    int32_t y0 = ctx->state.oy + cy - r;
    int32_t x1 = ctx->state.ox + cx + r;
    int32_t y1 = ctx->state.oy + cy + r;

    for (int32_t y = y0; y <= y1; y++) {
        for (int32_t x = x0; x <= x1; x++) {
            if (!ctx_clip_contains(ctx, x, y)) {
                continue;
            }

            float dx = ((float)x + 0.5f) - fcx;
            float dy = ((float)y + 0.5f) - fcy;
            float dist = sqrtf(dx * dx + dy * dy);
            uint8_t cov = arc_coverage(dist, (float)r);

            if (cov) {
                stlxgfx_blend_coverage(ctx->target, x, y, color, cov);
            }
        }
    }
}

void stlxgfx_ctx_fill_rounded_rect(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                                    uint32_t w, uint32_t h, uint32_t radius,
                                    uint32_t color) {
    if (!ctx || !ctx->target || w == 0 || h == 0) {
        return;
    }

    uint32_t max_r = (w < h ? w : h) / 2;
    if (radius > max_r) {
        radius = max_r;
    }

    if (radius == 0) {
        stlxgfx_ctx_fill_rect(ctx, x, y, w, h, color);
        return;
    }

    int32_t r = (int32_t)radius;

    stlxgfx_ctx_fill_rect(ctx, x + r, y, w - 2 * radius, h, color);
    stlxgfx_ctx_fill_rect(ctx, x, y + r, (uint32_t)r, h - 2 * radius, color);
    stlxgfx_ctx_fill_rect(ctx, x + (int32_t)w - r, y + r, (uint32_t)r, h - 2 * radius, color);

    // Anti-aliased corner quarters, one per rect corner
    int32_t xr = x + (int32_t)w;
    int32_t yb = y + (int32_t)h;
    stlxgfx_ctx_fill_arc_corner(ctx, x,  y,  radius, 0,  1,  1, 0, color);
    stlxgfx_ctx_fill_arc_corner(ctx, xr, y,  radius, 0, -1,  1, 0, color);
    stlxgfx_ctx_fill_arc_corner(ctx, x,  yb, radius, 0,  1, -1, 0, color);
    stlxgfx_ctx_fill_arc_corner(ctx, xr, yb, radius, 0, -1, -1, 0, color);
}

/* Plot one Wu line pixel: (major, minor) swaps back to (x, y) when the
 * line is steep, clips against the ctx clip rect, then blends. */
static void line_plot(stlxgfx_ctx_t *ctx, int32_t major, int32_t minor,
                       int steep, uint32_t color, uint8_t cov) {
    int32_t x = steep ? minor : major;
    int32_t y = steep ? major : minor;
    if (ctx_clip_contains(ctx, x, y)) {
        stlxgfx_blend_coverage(ctx->target, x, y, color, cov);
    }
}

void stlxgfx_ctx_draw_line(stlxgfx_ctx_t *ctx, int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1, uint32_t color) {
    if (!ctx || !ctx->target) {
        return;
    }

    // Axis-aligned lines are exact and go through the rect fill
    if (y0 == y1) {
        int32_t lo = x0 < x1 ? x0 : x1;
        uint32_t len = (uint32_t)(x0 < x1 ? x1 - x0 : x0 - x1) + 1;
        stlxgfx_ctx_fill_rect(ctx, lo, y0, len, 1, color);
        return;
    }

    if (x0 == x1) {
        int32_t lo = y0 < y1 ? y0 : y1;
        uint32_t len = (uint32_t)(y0 < y1 ? y1 - y0 : y0 - y1) + 1;
        stlxgfx_ctx_fill_rect(ctx, x0, lo, 1, len, color);
        return;
    }

    // Wu's anti-aliased line, integer endpoints
    int32_t ax0 = ctx->state.ox + x0;
    int32_t ay0 = ctx->state.oy + y0;
    int32_t ax1 = ctx->state.ox + x1;
    int32_t ay1 = ctx->state.oy + y1;

    int32_t adx = ax1 - ax0 < 0 ? ax0 - ax1 : ax1 - ax0;
    int32_t ady = ay1 - ay0 < 0 ? ay0 - ay1 : ay1 - ay0;
    int steep = ady > adx;

    if (steep) {
        int32_t t;
        t = ax0; ax0 = ay0; ay0 = t;
        t = ax1; ax1 = ay1; ay1 = t;
    }
    if (ax0 > ax1) {
        int32_t t;
        t = ax0; ax0 = ax1; ax1 = t;
        t = ay0; ay0 = ay1; ay1 = t;
    }

    float gradient = (float)(ay1 - ay0) / (float)(ax1 - ax0);

    line_plot(ctx, ax0, ay0, steep, color, 255);
    float intery = (float)ay0 + gradient;
    for (int32_t x = ax0 + 1; x < ax1; x++) {
        float fl = floorf(intery);
        uint8_t frac = (uint8_t)((intery - fl) * 255.0f);
        line_plot(ctx, x, (int32_t)fl,     steep, color, (uint8_t)(255 - frac));
        line_plot(ctx, x, (int32_t)fl + 1, steep, color, frac);
        intery += gradient;
    }
    line_plot(ctx, ax1, ay1, steep, color, 255);
}

void stlxgfx_ctx_draw_text(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                            const char *text, uint32_t font_size,
                            uint32_t color) {
    if (!ctx || !ctx->target) {
        return;
    }

    const stlxgfx_clip_t *c = &ctx->state.clip;
    if (c->w == 0 || c->h == 0) {
        return;
    }

    stlxgfx_draw_text_clipped(ctx->target,
                               ctx->state.ox + x, ctx->state.oy + y,
                               text, font_size, color,
                               c->x, c->y, c->w, c->h);
}

void stlxgfx_ctx_text_size(const char *text, uint32_t font_size,
                            uint32_t *out_w, uint32_t *out_h) {
    stlxgfx_text_size(text, font_size, out_w, out_h);
}

void stlxgfx_ctx_blit(stlxgfx_ctx_t *ctx, int32_t dx, int32_t dy,
                       const stlxgfx_surface_t *src, int32_t sx, int32_t sy,
                       uint32_t w, uint32_t h) {
    if (!ctx || !ctx->target || !src) {
        return;
    }

    int32_t adx = ctx->state.ox + dx;
    int32_t ady = ctx->state.oy + dy;

    const stlxgfx_clip_t *c = &ctx->state.clip;
    int32_t cx1 = c->x + (int32_t)c->w;
    int32_t cy1 = c->y + (int32_t)c->h;

    int32_t sw = (int32_t)w;
    int32_t sh = (int32_t)h;

    if (adx < c->x) {
        int32_t d = c->x - adx;
        sx += d;
        sw -= d;
        adx = c->x;
    }
    if (ady < c->y) {
        int32_t d = c->y - ady;
        sy += d;
        sh -= d;
        ady = c->y;
    }
    if (adx + sw > cx1) {
        sw = cx1 - adx;
    }
    if (ady + sh > cy1) {
        sh = cy1 - ady;
    }

    if (sw <= 0 || sh <= 0) {
        return;
    }

    stlxgfx_blit(ctx->target, adx, ady, src, sx, sy, (uint32_t)sw, (uint32_t)sh);
}

void stlxgfx_ctx_blit_alpha(stlxgfx_ctx_t *ctx, int32_t dx, int32_t dy,
                             const stlxgfx_surface_t *src, int32_t sx, int32_t sy,
                             uint32_t w, uint32_t h) {
    if (!ctx || !ctx->target || !src) {
        return;
    }

    int32_t adx = ctx->state.ox + dx;
    int32_t ady = ctx->state.oy + dy;

    const stlxgfx_clip_t *c = &ctx->state.clip;
    int32_t cx1 = c->x + (int32_t)c->w;
    int32_t cy1 = c->y + (int32_t)c->h;

    int32_t sw = (int32_t)w;
    int32_t sh = (int32_t)h;

    if (adx < c->x) {
        int32_t d = c->x - adx;
        sx += d;
        sw -= d;
        adx = c->x;
    }
    if (ady < c->y) {
        int32_t d = c->y - ady;
        sy += d;
        sh -= d;
        ady = c->y;
    }
    if (adx + sw > cx1) {
        sw = cx1 - adx;
    }
    if (ady + sh > cy1) {
        sh = cy1 - ady;
    }

    if (sw <= 0 || sh <= 0) {
        return;
    }

    stlxgfx_blit_alpha(ctx->target, adx, ady, src, sx, sy, (uint32_t)sw, (uint32_t)sh);
}
