#ifndef STLXDM_HUD_POWER_H
#define STLXDM_HUD_POWER_H

#include <stdint.h>
#include <stlxgfx/surface.h>
#include "stlxdm_hud.h"

// Power button component data structure
typedef struct stlxdm_hud_power_data {
    uint32_t button_size;       // Size of the power button (square)
    uint32_t normal_color;      // Normal button color
    uint32_t hover_color;       // Color when mouse is hovering
    uint32_t icon_color;        // Color of the power icon
    int is_hovered;             // Whether mouse is hovering over button
} stlxdm_hud_power_data_t;

// Power button component creation and management
/**
 * Create a power button component
 * @param component - component structure to initialize
 * @param x - X position
 * @param y - Y position  
 * @param width - button width
 * @param height - button height
 * @return 0 on success, negative on error
 */
int stlxdm_hud_power_create(stlxdm_hud_component_t* component, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void stlxdm_hud_power_destroy(stlxdm_hud_component_t* component);

// Power button component render and click handlers
int stlxdm_hud_power_render(stlxdm_hud_component_t* comp, stlxgfx_surface_t* surface, stlxgfx_context_t* gfx_ctx, int32_t mouse_x, int32_t mouse_y);
int stlxdm_hud_power_handle_click(stlxdm_hud_component_t* comp, int32_t click_x, int32_t click_y);

// Power button state management
int stlxdm_hud_power_set_hover_state(stlxdm_hud_component_t* component, int is_hovered);

#endif // STLXDM_HUD_POWER_H
