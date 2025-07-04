#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include "stlxdm.h"
#include "stlxdm_splash.h"
#include "stlxdm_hud.h"
#include <stlxgfx/stlxgfx.h>
#include <stlxgfx/internal/stlxgfx_dm.h>
#include <stlxgfx/internal/stlxgfx_comm.h>
#include <stlxgfx/internal/stlxgfx_protocol.h>
#include <stlibc/stlibc.h>

// ====================== //
//    Main Entry Point    //
// ====================== //

int main() {
    // === INITIALIZE GRAPHICS LIBRARY ===
    stlxgfx_context_t* gfx_ctx = stlxgfx_init(STLXGFX_MODE_DISPLAY_MANAGER);
    if (!gfx_ctx) {
        printf("ERROR: Failed to initialize graphics library\n");
        return 1;
    }
    
    // Initialize compositor
    stlxdm_compositor_t* compositor = malloc(sizeof(stlxdm_compositor_t));
    if (!compositor) {
        printf("ERROR: Failed to allocate compositor\n");
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }
    
    if (stlxdm_compositor_init(compositor, gfx_ctx, NULL) != 0) {
        printf("ERROR: Failed to initialize compositor\n");
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }

    // === SHOW SPLASH SCREEN ===
    if (stlxdm_show_splash_screen(compositor) != 0) {
        printf("ERROR: Failed to show splash screen\n");
        stlxdm_compositor_cleanup(compositor);
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }

    // Initialize display manager server (heap allocated)
    stlxdm_server_t* server = malloc(sizeof(stlxdm_server_t));
    if (!server) {
        printf("ERROR: Failed to allocate server\n");
        stlxdm_compositor_cleanup(compositor);
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }
    
    if (stlxdm_server_init(server, gfx_ctx, compositor->gop_format) != 0) {
        printf("ERROR: Failed to initialize display manager server\n");
        free(server);
        stlxdm_compositor_cleanup(compositor);
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }

    // Initialize HUD first
    stlxdm_hud_t* hud = malloc(sizeof(stlxdm_hud_t));
    if (!hud) {
        printf("ERROR: Failed to allocate HUD\n");
        stlxdm_server_cleanup(server);
        free(server);
        stlxdm_compositor_cleanup(compositor);
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }
    
    if (stlxdm_hud_init(hud, gfx_ctx) != 0) {
        printf("ERROR: Failed to initialize HUD\n");
        free(hud);
        stlxdm_server_cleanup(server);
        free(server);
        stlxdm_compositor_cleanup(compositor);
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }

    // Initialize input manager
    stlxdm_input_manager_t* input_manager = malloc(sizeof(stlxdm_input_manager_t));
    if (!input_manager) {
        printf("ERROR: Failed to allocate input manager\n");
        stlxdm_hud_cleanup(hud);
        free(hud);
        stlxdm_server_cleanup(server);
        free(server);
        stlxdm_compositor_cleanup(compositor);
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }
    
    if (stlxdm_input_manager_init(input_manager, compositor, server, hud) != 0) {
        printf("ERROR: Failed to initialize input manager\n");
        free(input_manager);
        stlxdm_hud_cleanup(hud);
        free(hud);
        stlxdm_server_cleanup(server);
        free(server);
        stlxdm_compositor_cleanup(compositor);
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }

    // Re-initialize compositor with HUD
    if (stlxdm_compositor_init(compositor, gfx_ctx, hud) != 0) {
        printf("ERROR: Failed to re-initialize compositor with HUD\n");
        stlxdm_hud_cleanup(hud);
        free(hud);
        stlxdm_input_manager_cleanup(input_manager);
        free(input_manager);
        stlxdm_server_cleanup(server);
        free(server);
        stlxdm_compositor_cleanup(compositor);
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }
    
    // Register default HUD components
    if (stlxdm_hud_register_default_components(hud, compositor->fb_info.width) != 0) {
        printf("ERROR: Failed to register default HUD components\n");
        stlxdm_hud_cleanup(hud);
        free(hud);
        stlxdm_input_manager_cleanup(input_manager);
        free(input_manager);
        stlxdm_server_cleanup(server);
        free(server);
        stlxdm_compositor_cleanup(compositor);
        free(compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }

    while (1) {
        // === INPUT EVENT HANDLING ===
        // Process input events using the input manager
        int events_processed = stlxdm_input_manager_process_events(input_manager);
        if (events_processed < 0) {
            printf("[STLXDM] Error processing input events\n");
        }

        // === CLIENT CONNECTION HANDLING ===
        // Accept new client connections
        int new_connections = stlxdm_server_accept_new_connections(server);
        if (new_connections < 0) {
            printf("[STLXDM] Error accepting client connections\n");
        }
        
        // === CLIENT MESSAGE HANDLING ===
        // Handle requests from all connected clients
        if (stlxdm_server_handle_client_requests(server) < 0) {
            printf("[STLXDM] Error handling client requests\n");
        }
        
        // === RENDERING/COMPOSITION ===
        // Get cursor position and focused window from input manager
        int32_t cursor_x = -1, cursor_y = -1;
        stlxdm_input_manager_get_cursor_position(input_manager, &cursor_x, &cursor_y);
        uint32_t focused_window_id = stlxdm_input_manager_get_focused_window_id(input_manager);
        
        // Handle HUD mouse events
        if (cursor_y >= 0 && cursor_y < STLXDM_HUD_HEIGHT) {
            stlxdm_hud_handle_mouse_move(hud, cursor_x, cursor_y);
        } else {
            // Mouse is outside HUD area - clear hover states
            stlxdm_hud_handle_mouse_move(hud, -1, -1);
        }
        
        // Compose the frame with cursor and focus information
        if (stlxdm_compositor_compose(compositor, server, cursor_x, cursor_y, focused_window_id) < 0) {
            printf("[STLXDM] Error composing frame\n");
        }
        
        // Present the composed frame to framebuffer
        if (stlxdm_compositor_present(compositor) < 0) {
            printf("[STLXDM] Error presenting frame\n");
        }
    }
        
    // Clean up HUD
    stlxdm_hud_cleanup(hud);
    free(hud);
    
    // Clean up input manager
    stlxdm_input_manager_cleanup(input_manager);
    free(input_manager);
    
    // Clean up server
    stlxdm_server_cleanup(server);
    free(server);
    
    // Clean up compositor
    stlxdm_compositor_cleanup(compositor);
    free(compositor);

    // Clean up graphics context
    stlxgfx_cleanup(gfx_ctx);
    return 0;
}
