#include <stlxgfx/ctx.h>
#include <stlxgfx/font.h>
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
    if (ctx_clip_rect(ctx, x, y, w, h, &ox, &oy, &ow, &oh)) {
        stlxgfx_fill_rect(ctx->target, ox, oy, ow, oh, color);
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

    int32_t r = (int32_t)radius;
    int32_t px = 0, py = r, d = 1 - r;

    stlxgfx_ctx_fill_rect(ctx, cx - r, cy, (uint32_t)(2 * r + 1), 1, color);
    while (px < py) {
        px++;
        if (d < 0) {
            d += 2 * px + 1;
        } else {
            py--;
            d += 2 * (px - py) + 1;
        }
        stlxgfx_ctx_fill_rect(ctx, cx - px, cy + py, (uint32_t)(2 * px + 1), 1, color);
        stlxgfx_ctx_fill_rect(ctx, cx - px, cy - py, (uint32_t)(2 * px + 1), 1, color);
        stlxgfx_ctx_fill_rect(ctx, cx - py, cy + px, (uint32_t)(2 * py + 1), 1, color);
        stlxgfx_ctx_fill_rect(ctx, cx - py, cy - px, (uint32_t)(2 * py + 1), 1, color);
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

    int32_t cx_tl = x + r, cy_tl = y + r;
    int32_t cx_tr = x + (int32_t)w - r - 1, cy_tr = y + r;
    int32_t cx_bl = x + r, cy_bl = y + (int32_t)h - r - 1;
    int32_t cx_br = x + (int32_t)w - r - 1, cy_br = y + (int32_t)h - r - 1;

    int32_t px = 0, py = r, d = 1 - r;
    while (px <= py) {
        stlxgfx_ctx_fill_rect(ctx, cx_tl - px, cy_tl - py, (uint32_t)(px + 1), 1, color);
        stlxgfx_ctx_fill_rect(ctx, cx_tr,      cy_tr - py, (uint32_t)(px + 1), 1, color);
        stlxgfx_ctx_fill_rect(ctx, cx_bl - px, cy_bl + py, (uint32_t)(px + 1), 1, color);
        stlxgfx_ctx_fill_rect(ctx, cx_br,      cy_br + py, (uint32_t)(px + 1), 1, color);

        stlxgfx_ctx_fill_rect(ctx, cx_tl - py, cy_tl - px, (uint32_t)(py + 1), 1, color);
        stlxgfx_ctx_fill_rect(ctx, cx_tr,      cy_tr - px, (uint32_t)(py + 1), 1, color);
        stlxgfx_ctx_fill_rect(ctx, cx_bl - py, cy_bl + px, (uint32_t)(py + 1), 1, color);
        stlxgfx_ctx_fill_rect(ctx, cx_br,      cy_br + px, (uint32_t)(py + 1), 1, color);

        px++;
        if (d < 0) {
            d += 2 * px + 1;
        } else {
            py--;
            d += 2 * (px - py) + 1;
        }
    }
}

void stlxgfx_ctx_draw_line(stlxgfx_ctx_t *ctx, int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1, uint32_t color) {
    if (!ctx || !ctx->target) {
        return;
    }

    int32_t ax0 = ctx->state.ox + x0;
    int32_t ay0 = ctx->state.oy + y0;
    int32_t ax1 = ctx->state.ox + x1;
    int32_t ay1 = ctx->state.oy + y1;

    const stlxgfx_clip_t *c = &ctx->state.clip;
    int32_t cxe = c->x + (int32_t)c->w;
    int32_t cye = c->y + (int32_t)c->h;

    int32_t ddx = ax1 - ax0;
    int32_t ddy = ay1 - ay0;
    int32_t abs_dx = ddx < 0 ? -ddx : ddx;
    int32_t abs_dy = ddy < 0 ? -ddy : ddy;
    int32_t sx = ddx < 0 ? -1 : 1;
    int32_t sy = ddy < 0 ? -1 : 1;

    uint32_t bytes_pp = ctx->target->bpp / 8;
    stlxgfx_surface_t *s = ctx->target;

    if (abs_dx >= abs_dy) {
        int32_t err = abs_dx / 2;
        int32_t y = ay0;
        for (int32_t x = ax0; x != ax1 + sx; x += sx) {
            if (x >= c->x && x < cxe && y >= c->y && y < cye) {
                uint8_t* px = s->pixels + (uint32_t)y * s->pitch + (uint32_t)x * bytes_pp;
                px[s->red_shift   / 8] = (color >> 16) & 0xFF;
                px[s->green_shift / 8] = (color >>  8) & 0xFF;
                px[s->blue_shift  / 8] =  color        & 0xFF;
                if (bytes_pp == 4) {
                    px[stlxgfx_alpha_byte_index(s)] = (color >> 24) & 0xFF;
                }
            }
            err -= abs_dy;
            if (err < 0) {
                y += sy;
                err += abs_dx;
            }
        }
    } else {
        int32_t err = abs_dy / 2;
        int32_t x = ax0;
        for (int32_t y = ay0; y != ay1 + sy; y += sy) {
            if (x >= c->x && x < cxe && y >= c->y && y < cye) {
                uint8_t* px = s->pixels + (uint32_t)y * s->pitch + (uint32_t)x * bytes_pp;
                px[s->red_shift   / 8] = (color >> 16) & 0xFF;
                px[s->green_shift / 8] = (color >>  8) & 0xFF;
                px[s->blue_shift  / 8] =  color        & 0xFF;
                if (bytes_pp == 4) {
                    px[stlxgfx_alpha_byte_index(s)] = (color >> 24) & 0xFF;
                }
            }
            err -= abs_dx;
            if (err < 0) {
                x += sx;
                err += abs_dy;
            }
        }
    }
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
