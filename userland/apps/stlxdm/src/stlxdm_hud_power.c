#define _POSIX_C_SOURCE 199309L
#include "stlxdm_hud_power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stlxgfx/stlxgfx.h>

int stlxdm_hud_power_create(stlxdm_hud_component_t* component, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!component) {
        printf("[STLXDM_HUD_POWER] ERROR: Invalid component parameter\n");
        return -1;
    }
    
    // Allocate power button-specific data
    stlxdm_hud_power_data_t* power_data = malloc(sizeof(stlxdm_hud_power_data_t));
    if (!power_data) {
        printf("[STLXDM_HUD_POWER] ERROR: Failed to allocate power button data\n");
        return -1;
    }
    
    // Initialize power button data
    memset(power_data, 0, sizeof(stlxdm_hud_power_data_t));
    power_data->button_size = width;  // Store width as button size
    power_data->normal_color = 0xFF404040;   // Dark gray background
    power_data->hover_color = 0xFF2A2A2A;    // Lighter gray on hover
    power_data->icon_color = 0xFFFFFFFF;     // White icon
    power_data->is_hovered = 0;
    
    // Initialize component structure
    component->id = 0;  // Will be set by HUD registration
    component->x = x;
    component->y = y;
    component->width = width;
    component->height = height;
    component->type = STLXDM_HUD_COMPONENT_POWER_BUTTON;
    component->data = power_data;
    component->render = stlxdm_hud_power_render;
    component->handle_click = stlxdm_hud_power_handle_click;
    
    return 0;
}

void stlxdm_hud_power_destroy(stlxdm_hud_component_t* component) {
    if (!component) {
        return;
    }
    
    // Free power button-specific data
    if (component->data) {
        free(component->data);
        component->data = NULL;
    }
}

int stlxdm_hud_power_render(stlxdm_hud_component_t* comp, stlxgfx_surface_t* surface, stlxgfx_context_t* gfx_ctx, int32_t mouse_x, int32_t mouse_y) {
    if (!comp || !comp->data || !surface) {
        return -1;
    }
    
    __unused gfx_ctx;  // Not used for this component
    
    stlxdm_hud_power_data_t* power_data = (stlxdm_hud_power_data_t*)comp->data;
    
    // Check if mouse is hovering over the button
    if (mouse_x >= 0 && mouse_y >= 0) {
        // Check if mouse is within the button bounds
        power_data->is_hovered = ((uint32_t)mouse_x >= comp->x && (uint32_t)mouse_x < comp->x + comp->width &&
                                  (uint32_t)mouse_y >= comp->y && (uint32_t)mouse_y < comp->y + comp->height);
    } else {
        // Mouse is outside HUD area or invalid - clear hover state
        power_data->is_hovered = 0;
    }
    
    // Choose background color based on hover state
    uint32_t bg_color = power_data->is_hovered ? power_data->hover_color : power_data->normal_color;
    
    // Draw button background (rounded rectangle)
    if (stlxgfx_fill_rounded_rect(surface, comp->x, comp->y, comp->width, comp->height, 4, bg_color) != 0) {
        printf("[STLXDM_HUD_POWER] ERROR: Failed to draw button background\n");
        return -1;
    }
    
    // Draw power icon (two vertical bars design)
    uint32_t center_x = comp->x + comp->width / 2;
    uint32_t center_y = comp->y + comp->height / 2;
    
    // Draw two vertical bars (power symbol)
    uint32_t bar_width = 3;
    uint32_t bar_height = comp->height / 2 - 4;  // Half the button height
    uint32_t bar_spacing = 6;  // Space between the two bars
    
    // Calculate positions for the two bars
    uint32_t left_bar_x = center_x - (bar_spacing / 2) - bar_width;
    uint32_t right_bar_x = center_x + (bar_spacing / 2);
    uint32_t bar_y = center_y - bar_height / 2;
    
    // Draw left bar
    if (stlxgfx_fill_rect(surface, left_bar_x, bar_y, bar_width, bar_height, power_data->icon_color) != 0) {
        printf("[STLXDM_HUD_POWER] ERROR: Failed to draw left power bar\n");
        return -1;
    }
    
    // Draw right bar
    if (stlxgfx_fill_rect(surface, right_bar_x, bar_y, bar_width, bar_height, power_data->icon_color) != 0) {
        printf("[STLXDM_HUD_POWER] ERROR: Failed to draw right power bar\n");
        return -1;
    }

    return 0;
}

int stlxdm_hud_power_handle_click(stlxdm_hud_component_t* comp, int32_t click_x, int32_t click_y) {
    if (!comp) {
        printf("[STLXDM_HUD_POWER] ERROR: Invalid component in click handler\n");
        return -1;
    }
    
    // Check if click is within button bounds
    if ((uint32_t)click_x >= comp->x && (uint32_t)click_x < comp->x + comp->width &&
        (uint32_t)click_y >= comp->y && (uint32_t)click_y < comp->y + comp->height) {
        
        printf("[STLXDM_HUD_POWER] Power button clicked - shutdown requested\n");
        
        // TODO: Implement actual shutdown functionality
        // For now, just print the message as specified
        
        return 0;  // Click handled
    }

    return -1;  // Click not handled
}

int stlxdm_hud_power_set_hover_state(stlxdm_hud_component_t* component, int is_hovered) {
    if (!component || !component->data) {
        return -1;
    }
    
    stlxdm_hud_power_data_t* power_data = (stlxdm_hud_power_data_t*)component->data;
    power_data->is_hovered = is_hovered;
    
    return 0;
}
