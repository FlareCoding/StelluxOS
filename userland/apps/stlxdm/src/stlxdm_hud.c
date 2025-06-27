#include "stlxdm_hud.h"
#include "stlxdm_hud_clock.h"
#include "stlxdm_hud_power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stlxgfx/stlxgfx.h>

int stlxdm_hud_init(stlxdm_hud_t* hud, stlxgfx_context_t* gfx_ctx) {
    if (!hud || !gfx_ctx) {
        printf("[STLXDM_HUD] ERROR: Invalid parameters for HUD init\n");
        return -1;
    }
    
    // Initialize HUD structure
    memset(hud, 0, sizeof(stlxdm_hud_t));
    
    // Allocate component array
    hud->components = malloc(sizeof(stlxdm_hud_component_t) * STLXDM_HUD_MAX_COMPONENTS);
    if (!hud->components) {
        printf("[STLXDM_HUD] ERROR: Failed to allocate component array\n");
        return -1;
    }
    
    // Initialize component array
    memset(hud->components, 0, sizeof(stlxdm_hud_component_t) * STLXDM_HUD_MAX_COMPONENTS);
    
    // Set up HUD properties
    hud->gfx_ctx = gfx_ctx;
    hud->max_components = STLXDM_HUD_MAX_COMPONENTS;
    hud->component_count = 0;
    hud->background_color = 0xFF1A1A1A;  // Slightly lighter than background
    hud->hover_color = 0xFF2A2A2A;       // Even lighter for hover effects
    hud->mouse_x = -1;
    hud->mouse_y = -1;
    hud->mouse_over_hud = 0;
    hud->needs_redraw = 1;  // Initial render needed
    
    printf("[STLXDM_HUD] HUD initialized successfully\n");
    return 0;
}

void stlxdm_hud_cleanup(stlxdm_hud_t* hud) {
    if (!hud) {
        return;
    }
    
    // Free component array
    if (hud->components) {
        free(hud->components);
        hud->components = NULL;
    }
    
    // Reset HUD state
    hud->component_count = 0;
    hud->max_components = 0;
    hud->needs_redraw = 0;
    
    printf("[STLXDM_HUD] HUD cleaned up\n");
}

int stlxdm_hud_register_component(stlxdm_hud_t* hud, stlxdm_hud_component_t* component) {
    if (!hud || !component) {
        printf("[STLXDM_HUD] ERROR: Invalid parameters for component registration\n");
        return -1;
    }
    
    if (hud->component_count >= hud->max_components) {
        printf("[STLXDM_HUD] ERROR: Maximum component count reached\n");
        return -1;
    }
    
    // Assign unique ID to component
    component->id = hud->component_count + 1000;  // Start IDs at 1000
    
    // Add component to array
    hud->components[hud->component_count] = *component;
    hud->component_count++;
    
    // Mark HUD as needing redraw
    stlxdm_hud_mark_dirty(hud);
    
    printf("[STLXDM_HUD] Registered component ID=%u, type=%d\n", component->id, component->type);
    return 0;
}

int stlxdm_hud_unregister_component(stlxdm_hud_t* hud, uint32_t component_id) {
    if (!hud) {
        return -1;
    }
    
    // Find component by ID
    for (uint32_t i = 0; i < hud->component_count; i++) {
        if (hud->components[i].id == component_id) {
            // Remove component by shifting remaining components
            for (uint32_t j = i; j < hud->component_count - 1; j++) {
                hud->components[j] = hud->components[j + 1];
            }
            hud->component_count--;
            
            // Mark HUD as needing redraw
            stlxdm_hud_mark_dirty(hud);
            
            printf("[STLXDM_HUD] Unregistered component ID=%u\n", component_id);
            return 0;
        }
    }
    
    printf("[STLXDM_HUD] Component ID=%u not found for unregistration\n", component_id);
    return -1;
}

int stlxdm_hud_render(stlxdm_hud_t* hud, stlxgfx_surface_t* surface) {
    if (!hud || !surface) {
        printf("[STLXDM_HUD] ERROR: Invalid parameters for HUD render\n");
        return -1;
    }
    
    // Render HUD background
    if (stlxgfx_fill_rect(surface, 0, 0, surface->width, STLXDM_HUD_HEIGHT, hud->background_color) != 0) {
        printf("[STLXDM_HUD] ERROR: Failed to render HUD background\n");
        return -1;
    }
    
    // Render all components
    for (uint32_t i = 0; i < hud->component_count; i++) {
        stlxdm_hud_component_t* comp = &hud->components[i];
        if (comp->render) {
            if (comp->render(comp, surface, hud->gfx_ctx, hud->mouse_x, hud->mouse_y) != 0) {
                printf("[STLXDM_HUD] ERROR: Failed to render component ID=%u\n", comp->id);
            }
        }
    }
    
    // Clear dirty flag after successful render
    hud->needs_redraw = 0;
    
    return 0;
}

int stlxdm_hud_handle_mouse_move(stlxdm_hud_t* hud, int32_t mouse_x, int32_t mouse_y) {
    if (!hud) {
        return -1;
    }
    
    // Check if mouse is over HUD area
    int was_over_hud = hud->mouse_over_hud;
    hud->mouse_over_hud = (mouse_y >= 0 && mouse_y < STLXDM_HUD_HEIGHT);
    
    // Update mouse position
    hud->mouse_x = mouse_x;
    hud->mouse_y = mouse_y;
    
    // Mark dirty if hover state changed
    if (was_over_hud != hud->mouse_over_hud) {
        stlxdm_hud_mark_dirty(hud);
    }
    
    return 0;
}

int stlxdm_hud_handle_mouse_click(stlxdm_hud_t* hud, int32_t click_x, int32_t click_y) {
    if (!hud) {
        printf("[STLXDM_HUD] ERROR: Invalid HUD in click handler\n");
        return -1;
    }

    // Check if click is within HUD area
    if (click_y < 0 || click_y >= STLXDM_HUD_HEIGHT) {
        printf("[STLXDM_HUD] Click outside HUD area (y=%d, HUD height=%d)\n", click_y, STLXDM_HUD_HEIGHT);
        return 0;  // Click outside HUD, not handled
    }

    // Check each component for click handling
    for (uint32_t i = 0; i < hud->component_count; i++) {
        stlxdm_hud_component_t* comp = &hud->components[i];
        
        // Check if click is within component bounds
        if ((uint32_t)click_x >= comp->x && (uint32_t)click_x < comp->x + comp->width &&
            (uint32_t)click_y >= comp->y && (uint32_t)click_y < comp->y + comp->height) {
            if (comp->handle_click) {
                if (comp->handle_click(comp, click_x, click_y) == 0) {
                    return 0;  // Click handled
                }
            }
        }
    }

    return 0;  // Click not handled by any component
}

void stlxdm_hud_mark_dirty(stlxdm_hud_t* hud) {
    if (hud) {
        hud->needs_redraw = 1;
    }
}

int stlxdm_hud_needs_redraw(const stlxdm_hud_t* hud) {
    return hud ? hud->needs_redraw : 0;
}

int stlxdm_hud_register_default_components(stlxdm_hud_t* hud, uint32_t screen_width) {
    if (!hud) {
        printf("[STLXDM_HUD] ERROR: Invalid HUD for component registration\n");
        return -1;
    }

    // Register clock component (centered)
    stlxdm_hud_component_t clock_component;
    uint32_t clock_width = 100;
    uint32_t clock_height = 20;
    uint32_t clock_x = (screen_width / 2) - (clock_width / 2);  // Center horizontally
    uint32_t clock_y = (STLXDM_HUD_HEIGHT / 2) - (clock_height / 2);  // Center vertically in HUD

    if (stlxdm_hud_clock_create(&clock_component, clock_x, clock_y, clock_width, clock_height) == 0) {
        if (stlxdm_hud_register_component(hud, &clock_component) != 0) {
            printf("[STLXDM_HUD] Failed to register clock component\n");
            return -1;
        }
    } else {
        printf("[STLXDM_HUD] Failed to create clock component\n");
        return -1;
    }

    // Register power button component (top-right corner)
    stlxdm_hud_component_t power_component;
    uint32_t power_size = 32;  // 32px width
    uint32_t power_x = screen_width - power_size;  // Right-aligned with no margin
    uint32_t power_y = 0;  // Start at top of HUD
    uint32_t power_height = STLXDM_HUD_HEIGHT;  // Full HUD height
    
    if (stlxdm_hud_power_create(&power_component, power_x, power_y, power_size, power_height) == 0) {
        if (stlxdm_hud_register_component(hud, &power_component) != 0) {
            printf("[STLXDM_HUD] Failed to register power button component\n");
            return -1;
        }
    } else {
        printf("[STLXDM_HUD] Failed to create power button component\n");
        return -1;
    }

    return 0;
}
