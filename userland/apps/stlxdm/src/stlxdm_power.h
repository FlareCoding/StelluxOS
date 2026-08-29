#ifndef STLXDM_POWER_H
#define STLXDM_POWER_H

#include "stlxdm_conf.h"
#include <stlxgfx/ctx.h>
#include <stdint.h>

/* Choices offered by the overlay, also the index of each orb */
#define STLXDM_POWER_RESTART   0
#define STLXDM_POWER_SHUTDOWN  1

typedef enum {
    STLXDM_POWER_CLOSED = 0,
    STLXDM_POWER_OPENING,
    STLXDM_POWER_OPEN,
    STLXDM_POWER_CLOSING,
    STLXDM_POWER_COMMITTING
} stlxdm_power_state_t;

typedef struct {
    const stlxdm_config_t* conf;
    uint32_t fb_width;
    uint32_t fb_height;

    stlxdm_power_state_t state;
    uint64_t phase_start_ns;

    int32_t  star_cx;
    int32_t  star_cy;
    int      star_hover;

    int      hover_choice;   /* -1 when the pointer is off both orbs */
    int      press_choice;
    uint64_t hold_start_ns;

    int      commit_choice;
    int      action_ready;   /* the collapse finished, safe to power off */

    /* The dimmed desktop is identical every frame the menu sits open, so it
     * is composed once and only the orb boxes are repainted after that. */
    stlxgfx_surface_t* backdrop;
    int      backdrop_valid;
} stlxdm_power_t;

void stlxdm_power_init(stlxdm_power_t* pw, const stlxdm_config_t* conf,
                        uint32_t fb_width, uint32_t fb_height);

/* True while the overlay owns input and the screen needs a full redraw */
int  stlxdm_power_is_active(const stlxdm_power_t* pw);

/* True while any animation is running, so frames keep being produced */
int  stlxdm_power_is_animating(const stlxdm_power_t* pw);

int  stlxdm_power_star_hit(const stlxdm_power_t* pw, int32_t px, int32_t py);

void stlxdm_power_open(stlxdm_power_t* pw);
void stlxdm_power_dismiss(stlxdm_power_t* pw);

void stlxdm_power_on_motion(stlxdm_power_t* pw, int32_t px, int32_t py);
void stlxdm_power_on_press(stlxdm_power_t* pw, int32_t px, int32_t py);
void stlxdm_power_on_release(stlxdm_power_t* pw, int32_t px, int32_t py);

/* Advances timers and completes a held confirmation */
void stlxdm_power_update(stlxdm_power_t* pw);

void stlxdm_power_draw_star(stlxdm_power_t* pw, stlxgfx_ctx_t* ctx);
void stlxdm_power_draw_overlay(stlxdm_power_t* pw, stlxgfx_ctx_t* ctx);

/* True once the cached backdrop is usable, so the frame can skip composing
 * the desktop and repaint only what actually changes */
int  stlxdm_power_is_steady(const stlxdm_power_t* pw);

/* Bounding box of one orb including its glow, hold ring, and label */
void stlxdm_power_orb_box(const stlxdm_power_t* pw, int choice,
                           int32_t* x, int32_t* y, uint32_t* w, uint32_t* h);

void stlxdm_power_restore(stlxdm_power_t* pw, stlxgfx_surface_t* dst,
                           int32_t x, int32_t y, uint32_t w, uint32_t h);
void stlxdm_power_draw_orbs(stlxdm_power_t* pw, stlxgfx_ctx_t* ctx);

/* Issues the chosen power operation, only returns if the platform refused */
void stlxdm_power_run_action(stlxdm_power_t* pw);

#endif /* STLXDM_POWER_H */
