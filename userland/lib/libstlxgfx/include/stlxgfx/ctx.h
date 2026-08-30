#ifndef STLXGFX_CTX_H
#define STLXGFX_CTX_H

#include <stlxgfx/surface.h>

#define STLXGFX_CTX_MAX_SAVE_DEPTH 16

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t  x, y;
    uint32_t w, h;
} stlxgfx_clip_t;

typedef struct {
    int32_t        ox, oy;
    stlxgfx_clip_t clip;
} stlxgfx_ctx_state_t;

typedef struct {
    stlxgfx_surface_t  *target;
    stlxgfx_ctx_state_t state;
    stlxgfx_ctx_state_t stack[STLXGFX_CTX_MAX_SAVE_DEPTH];
    int                  stack_depth;
} stlxgfx_ctx_t;

void stlxgfx_ctx_init(stlxgfx_ctx_t *ctx, stlxgfx_surface_t *target);

int  stlxgfx_ctx_save(stlxgfx_ctx_t *ctx);
int  stlxgfx_ctx_restore(stlxgfx_ctx_t *ctx);

void stlxgfx_ctx_translate(stlxgfx_ctx_t *ctx, int32_t dx, int32_t dy);

void stlxgfx_ctx_clip(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                       uint32_t w, uint32_t h);
void stlxgfx_ctx_reset_clip(stlxgfx_ctx_t *ctx);

void stlxgfx_ctx_clear(stlxgfx_ctx_t *ctx, uint32_t color);
void stlxgfx_ctx_fill_rect(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t color);
void stlxgfx_ctx_draw_rect(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t color);
void stlxgfx_ctx_fill_circle(stlxgfx_ctx_t *ctx, int32_t cx, int32_t cy,
                              uint32_t radius, uint32_t color);
void stlxgfx_ctx_fill_rounded_rect(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                                    uint32_t w, uint32_t h, uint32_t radius,
                                    uint32_t color);
void stlxgfx_ctx_draw_line(stlxgfx_ctx_t *ctx, int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1, uint32_t color);

/* Anti-aliased corner arc fill for the r_outer-by-r_outer pixel box at
 * rect corner (x, y), growing inward along (dir_x, dir_y) (+1 or -1
 * each). Fills inside the arc, a nonzero r_inner leaves the concentric
 * inner disc empty (border ring), and invert fills outside the arc
 * instead (squaring off or erasing a rounded corner region). */
void stlxgfx_ctx_fill_arc_corner(stlxgfx_ctx_t *ctx, int32_t x, int32_t y,
                                  uint32_t r_outer, uint32_t r_inner,
                                  int dir_x, int dir_y, int invert,
                                  uint32_t color);
void stlxgfx_ctx_blit(stlxgfx_ctx_t *ctx, int32_t dx, int32_t dy,
                       const stlxgfx_surface_t *src, int32_t sx, int32_t sy,
                       uint32_t w, uint32_t h);
void stlxgfx_ctx_blit_alpha(stlxgfx_ctx_t *ctx, int32_t dx, int32_t dy,
                             const stlxgfx_surface_t *src, int32_t sx, int32_t sy,
                             uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif /* STLXGFX_CTX_H */
