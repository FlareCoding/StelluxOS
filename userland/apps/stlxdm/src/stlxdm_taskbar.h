#ifndef STLXDM_TASKBAR_H
#define STLXDM_TASKBAR_H

#include "stlxdm_conf.h"
#include <stlxgfx/ctx.h>
#include <stdint.h>

#define STLXDM_TASKBAR_MAX_ITEMS     STLXDM_CONF_MAX_TASKBAR

#define STLXDM_TASKBAR_ICON_SIZE     40
#define STLXDM_TASKBAR_ICON_RADIUS   10
#define STLXDM_TASKBAR_PADDING       8
#define STLXDM_TASKBAR_MARGIN_BOTTOM 10
#define STLXDM_TASKBAR_DOCK_RADIUS   14
#define STLXDM_TASKBAR_DOCK_PADDING  6

#define STLXDM_TASKBAR_TOOLTIP_FONT  12
#define STLXDM_TASKBAR_TOOLTIP_PAD_X 10
#define STLXDM_TASKBAR_TOOLTIP_PAD_Y 5
#define STLXDM_TASKBAR_TOOLTIP_RAD   6
#define STLXDM_TASKBAR_TOOLTIP_GAP   6

#define STLXDM_TASKBAR_ICON_BG           0xFF45475A
#define STLXDM_TASKBAR_ICON_HOVER_BG     0xFF585B70
#define STLXDM_TASKBAR_ICON_PRESS_BG     0xFF6C7086
#define STLXDM_TASKBAR_ICON_TEXT_COLOR   0xFFCDD6F4
#define STLXDM_TASKBAR_DOCK_BG           0xCC1E1E2E
#define STLXDM_TASKBAR_DOCK_BORDER       0xFF313244
#define STLXDM_TASKBAR_TOOLTIP_BG        0xEE1E1E2E
#define STLXDM_TASKBAR_TOOLTIP_BORDER    0xFF585B70
#define STLXDM_TASKBAR_TOOLTIP_TEXT      0xFFCDD6F4

typedef struct {
    char     label[64];
    char     path[256];
    int32_t  x;
    int32_t  y;
} stlxdm_taskbar_item_t;

typedef struct stlxdm_taskbar_t {
    stlxdm_taskbar_item_t items[STLXDM_TASKBAR_MAX_ITEMS];
    int                   count;

    int32_t  dock_x;
    int32_t  dock_y;
    uint32_t dock_w;
    uint32_t dock_h;

    int      hover_index;
    int      press_index;
    int      launch_index;
} stlxdm_taskbar_t;

void stlxdm_taskbar_init(stlxdm_taskbar_t* tb);
void stlxdm_taskbar_load(stlxdm_taskbar_t* tb, const stlxdm_conf_t* conf,
                          uint32_t screen_w, uint32_t screen_h);
void stlxdm_taskbar_draw(stlxdm_taskbar_t* tb, stlxgfx_ctx_t* ctx);
int  stlxdm_taskbar_hit_test(const stlxdm_taskbar_t* tb,
                              int32_t px, int32_t py);
int  stlxdm_taskbar_contains(const stlxdm_taskbar_t* tb,
                               int32_t px, int32_t py);
void stlxdm_taskbar_set_hover(stlxdm_taskbar_t* tb, int32_t px, int32_t py);

#endif /* STLXDM_TASKBAR_H */
