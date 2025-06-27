#ifndef STLXDM_HUD_CLOCK_H
#define STLXDM_HUD_CLOCK_H

#include <stdint.h>
#include <stlxgfx/surface.h>
#include "stlxdm_hud.h"

// Clock component data structure
typedef struct stlxdm_hud_clock_data {
    uint32_t last_update_time;  // Last time we updated the display
    char time_string[16];       // Formatted time string (e.g., "12:34:56")
    uint32_t font_size;         // Font size for rendering
    uint32_t text_color;        // Text color
    int needs_update;           // Flag to indicate if time needs updating
    uint32_t cached_text_x;     // Cached X position for text rendering
    uint32_t cached_text_y;     // Cached Y position for text rendering
    int position_cached;        // Flag to indicate if position is cached
} stlxdm_hud_clock_data_t;

// Clock component creation and management
int stlxdm_hud_clock_create(stlxdm_hud_component_t* component, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void stlxdm_hud_clock_destroy(stlxdm_hud_component_t* component);

// Clock component render and click handlers
int stlxdm_hud_clock_render(stlxdm_hud_component_t* comp, stlxgfx_surface_t* surface, stlxgfx_context_t* gfx_ctx, int32_t mouse_x, int32_t mouse_y);
int stlxdm_hud_clock_handle_click(stlxdm_hud_component_t* comp, int32_t click_x, int32_t click_y);

// Clock update functions
int stlxdm_hud_clock_update_time(stlxdm_hud_component_t* component);
int stlxdm_hud_clock_needs_update(const stlxdm_hud_component_t* component);

#endif // STLXDM_HUD_CLOCK_H
