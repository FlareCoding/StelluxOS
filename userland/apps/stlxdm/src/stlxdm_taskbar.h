#ifndef STLXDM_TASKBAR_H
#define STLXDM_TASKBAR_H

#include "stlxdm_conf.h"
#include <stlxgfx/ctx.h>
#include <stlxgfx/surface.h>
#include <stdint.h>

typedef struct stlxdm_taskbar_t_tag {
    const stlxdm_config_t* conf;
    int32_t  bar_y;
    uint32_t fb_width;
    uint32_t fb_height;
    int      hover_index;
    int      press_index;
    char     launch_path[256];

    stlxgfx_surface_t* icon_surfaces[STLXDM_CONF_MAX_TASKBAR_ITEMS];
    stlxgfx_surface_t* default_icon;
} stlxdm_taskbar_t;

void stlxdm_taskbar_init(stlxdm_taskbar_t* tb, const stlxdm_config_t* conf,
                          uint32_t fb_width, uint32_t fb_height);
void stlxdm_taskbar_cleanup(stlxdm_taskbar_t* tb);
void stlxdm_taskbar_draw(stlxdm_taskbar_t* tb, stlxgfx_ctx_t* ctx);
int  stlxdm_taskbar_hit_test(const stlxdm_taskbar_t* tb,
                              int32_t px, int32_t py);
void stlxdm_taskbar_on_press(stlxdm_taskbar_t* tb, int32_t px, int32_t py);
void stlxdm_taskbar_on_release(stlxdm_taskbar_t* tb, int32_t px, int32_t py);
void stlxdm_taskbar_update_hover(stlxdm_taskbar_t* tb,
                                  int32_t px, int32_t py);

#endif /* STLXDM_TASKBAR_H */
