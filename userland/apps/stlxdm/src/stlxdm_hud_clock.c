#define _POSIX_C_SOURCE 199309L
#include "stlxdm_hud_clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stlxgfx/stlxgfx.h>
#include <stlxgfx/font.h>

// Forward declaration
static int stlxdm_hud_clock_calculate_position(stlxdm_hud_component_t* component, stlxgfx_context_t* gfx_ctx);

int stlxdm_hud_clock_create(stlxdm_hud_component_t* component, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!component) {
        printf("[STLXDM_HUD_CLOCK] ERROR: Invalid component parameter\n");
        return -1;
    }
    
    // Allocate clock-specific data
    stlxdm_hud_clock_data_t* clock_data = malloc(sizeof(stlxdm_hud_clock_data_t));
    if (!clock_data) {
        printf("[STLXDM_HUD_CLOCK] ERROR: Failed to allocate clock data\n");
        return -1;
    }
    
    // Initialize clock data
    memset(clock_data, 0, sizeof(stlxdm_hud_clock_data_t));
    clock_data->font_size = 14;  // Font size as specified
    clock_data->text_color = 0xFFFFFFFF;  // White text
    clock_data->needs_update = 1;  // Initial update needed
    clock_data->position_cached = 0;  // Position not cached yet
    strcpy(clock_data->time_string, "00:00:00");  // Default time string
    
    // Initialize component structure
    component->id = 0;  // Will be set by HUD registration
    component->x = x;
    component->y = y;
    component->width = width;
    component->height = height;
    component->type = STLXDM_HUD_COMPONENT_CLOCK;
    component->data = clock_data;
    component->render = stlxdm_hud_clock_render;
    component->handle_click = stlxdm_hud_clock_handle_click;

    return 0;
}

void stlxdm_hud_clock_destroy(stlxdm_hud_component_t* component) {
    if (!component) {
        return;
    }
    
    // Free clock-specific data
    if (component->data) {
        free(component->data);
        component->data = NULL;
    }
}

int stlxdm_hud_clock_render(stlxdm_hud_component_t* comp, stlxgfx_surface_t* surface, stlxgfx_context_t* gfx_ctx, int32_t mouse_x, int32_t mouse_y) {
    if (!comp || !comp->data || !surface || !gfx_ctx) {
        return -1;
    }
    
    __unused mouse_x; __unused mouse_y;
    
    stlxdm_hud_clock_data_t* clock_data = (stlxdm_hud_clock_data_t*)comp->data;
    
    // Check if we need to update the time
    if (stlxdm_hud_clock_needs_update(comp)) {
        stlxdm_hud_clock_update_time(comp);
        // Invalidate position cache since text changed
        clock_data->position_cached = 0;
    }
    
    // Calculate position if not cached or if text changed
    if (!clock_data->position_cached) {
        if (stlxdm_hud_clock_calculate_position(comp, gfx_ctx) != 0) {
            printf("[STLXDM_HUD_CLOCK] ERROR: Failed to calculate position\n");
            return -1;
        }
    }
    
    // Render the time text using the cached position
    if (stlxgfx_render_text(gfx_ctx, surface, clock_data->time_string, 
                           clock_data->cached_text_x, clock_data->cached_text_y, 
                           clock_data->font_size, clock_data->text_color) != 0) {
        printf("[STLXDM_HUD_CLOCK] ERROR: Failed to render text\n");
        return -1;
    }
    
    return 0;
}

int stlxdm_hud_clock_handle_click(stlxdm_hud_component_t* comp, int32_t click_x, int32_t click_y) {
    if (!comp) {
        return -1;
    }
    
    // Clock component doesn't handle clicks for now
    // Could be extended to show date, timezone, etc.
    printf("[STLXDM_HUD_CLOCK] Clock clicked at (%d, %d)\n", click_x, click_y);
    
    return 0;  // Click handled
}

int stlxdm_hud_clock_update_time(stlxdm_hud_component_t* component) {
    if (!component || !component->data) {
        return -1;
    }
    
    stlxdm_hud_clock_data_t* clock_data = (stlxdm_hud_clock_data_t*)component->data;
    
    // Get current system uptime using clock_gettime
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        printf("[STLXDM_HUD_CLOCK] ERROR: Failed to get system time\n");
        return -1;
    }
    
    // Convert to hours, minutes, seconds
    uint32_t total_seconds = (uint32_t)ts.tv_sec;
    uint32_t hours = total_seconds / 3600;
    uint32_t minutes = (total_seconds % 3600) / 60;
    uint32_t seconds = total_seconds % 60;
    
    // Format time string as hh:mm:ss
    snprintf(clock_data->time_string, sizeof(clock_data->time_string), 
             "%02u:%02u:%02u", hours, minutes, seconds);
    
    // Update last update time
    clock_data->last_update_time = total_seconds;
    clock_data->needs_update = 0;
    
    return 0;
}

int stlxdm_hud_clock_needs_update(const stlxdm_hud_component_t* component) {
    if (!component || !component->data) {
        return 0;
    }
    
    stlxdm_hud_clock_data_t* clock_data = (stlxdm_hud_clock_data_t*)component->data;
    
    // Check if we need to update (every second)
    if (clock_data->needs_update) {
        return 1;
    }
    
    // Get current time and check if a second has passed
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;  // Don't update if we can't get time
    }
    
    uint32_t current_seconds = (uint32_t)ts.tv_sec;
    return (current_seconds > clock_data->last_update_time);
}

static int stlxdm_hud_clock_calculate_position(stlxdm_hud_component_t* component, stlxgfx_context_t* gfx_ctx) {
    if (!component || !component->data || !gfx_ctx) {
        return -1;
    }
    
    stlxdm_hud_clock_data_t* clock_data = (stlxdm_hud_clock_data_t*)component->data;
    
    // Get text dimensions using graphics library
    stlxgfx_text_size_t text_size;
    if (stlxgfx_get_text_size(gfx_ctx, clock_data->time_string, clock_data->font_size, &text_size) != 0) {
        printf("[STLXDM_HUD_CLOCK] ERROR: Failed to get text size\n");
        return -1;
    }
    
    // Calculate centered position
    clock_data->cached_text_x = component->x + (component->width / 2) - (text_size.width / 2);
    clock_data->cached_text_y = component->y + (component->height / 2) - (text_size.height / 2);
    clock_data->position_cached = 1;
    
    return 0;
}
