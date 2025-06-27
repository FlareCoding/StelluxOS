#ifndef STLXDM_HUD_H
#define STLXDM_HUD_H

#include <stdint.h>
#include <stlxgfx/surface.h>

// Forward declarations
typedef struct stlxgfx_context stlxgfx_context_t;

// HUD Component Types
typedef enum {
    STLXDM_HUD_COMPONENT_CLOCK,
    STLXDM_HUD_COMPONENT_POWER_BUTTON,
    // Future components can be added here
} stlxdm_hud_component_type_t;

// HUD Component Structure
typedef struct stlxdm_hud_component {
    uint32_t id;
    uint32_t x, y, width, height;
    stlxdm_hud_component_type_t type;
    void* data;  // Component-specific data
    int (*render)(struct stlxdm_hud_component* comp, stlxgfx_surface_t* surface, stlxgfx_context_t* gfx_ctx, int32_t mouse_x, int32_t mouse_y);
    int (*handle_click)(struct stlxdm_hud_component* comp, int32_t click_x, int32_t click_y);
} stlxdm_hud_component_t;

// HUD Manager Structure
typedef struct stlxdm_hud {
    stlxgfx_context_t* gfx_ctx;
    stlxdm_hud_component_t* components;
    uint32_t component_count;
    uint32_t max_components;
    uint32_t background_color;
    uint32_t hover_color;
    int32_t mouse_x, mouse_y;
    int mouse_over_hud;
    int needs_redraw;
} stlxdm_hud_t;

// HUD Constants
#define STLXDM_HUD_HEIGHT 40
#define STLXDM_HUD_MAX_COMPONENTS 16

// HUD API Functions
int stlxdm_hud_init(stlxdm_hud_t* hud, stlxgfx_context_t* gfx_ctx);
void stlxdm_hud_cleanup(stlxdm_hud_t* hud);

int stlxdm_hud_register_component(stlxdm_hud_t* hud, stlxdm_hud_component_t* component);
int stlxdm_hud_unregister_component(stlxdm_hud_t* hud, uint32_t component_id);

int stlxdm_hud_render(stlxdm_hud_t* hud, stlxgfx_surface_t* surface);
int stlxdm_hud_handle_mouse_move(stlxdm_hud_t* hud, int32_t mouse_x, int32_t mouse_y);
int stlxdm_hud_handle_mouse_click(stlxdm_hud_t* hud, int32_t click_x, int32_t click_y);

void stlxdm_hud_mark_dirty(stlxdm_hud_t* hud);
int stlxdm_hud_needs_redraw(const stlxdm_hud_t* hud);

/**
 * Register all default HUD components
 * @param hud - HUD context
 * @param screen_width - screen width for component positioning
 * @return 0 on success, negative on error
 */
int stlxdm_hud_register_default_components(stlxdm_hud_t* hud, uint32_t screen_width);

#endif // STLXDM_HUD_H
