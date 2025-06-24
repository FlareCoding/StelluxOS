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
    stlxdm_compositor_t compositor;
    if (stlxdm_compositor_init(&compositor, gfx_ctx) != 0) {
        printf("ERROR: Failed to initialize compositor\n");
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }

    // Initialize display manager server
    stlxdm_server_t server;
    if (stlxdm_server_init(&server, gfx_ctx, compositor.gop_format) != 0) {
        printf("ERROR: Failed to initialize display manager server\n");
        stlxdm_compositor_cleanup(&compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }

    // Initialize input manager (after compositor and server)
    stlxdm_input_manager_t input_manager;
    if (stlxdm_input_manager_init(&input_manager, &compositor, &server) != 0) {
        printf("ERROR: Failed to initialize input manager\n");
        stlxdm_server_cleanup(&server);
        stlxdm_compositor_cleanup(&compositor);
        stlxgfx_cleanup(gfx_ctx);
        return 1;
    }
    
    while (1) {
        // === INPUT EVENT HANDLING ===
        // Process input events using the input manager
        int events_processed = stlxdm_input_manager_process_events(&input_manager);
        if (events_processed < 0) {
            printf("[STLXDM] Error processing input events\n");
        }

        // === CLIENT CONNECTION HANDLING ===
        // Accept new client connections
        int new_connections = stlxdm_server_accept_new_connections(&server);
        if (new_connections < 0) {
            printf("[STLXDM] Error accepting client connections\n");
        }
        
        // === CLIENT MESSAGE HANDLING ===
        // Handle requests from all connected clients
        if (stlxdm_server_handle_client_requests(&server) < 0) {
            printf("[STLXDM] Error handling client requests\n");
        }
        
        // === RENDERING/COMPOSITION ===
        // Get cursor position from input manager
        int32_t cursor_x = -1, cursor_y = -1;
        stlxdm_input_manager_get_cursor_position(&input_manager, &cursor_x, &cursor_y);
        
        // Compose the frame with cursor
        if (stlxdm_compositor_compose(&compositor, &server, cursor_x, cursor_y) < 0) {
            printf("[STLXDM] Error composing frame\n");
        }
        
        // Present the composed frame to framebuffer
        if (stlxdm_compositor_present(&compositor) < 0) {
            printf("[STLXDM] Error presenting frame\n");
        }
    }
        
    // Clean up input manager
    stlxdm_input_manager_cleanup(&input_manager);
    
    // Clean up server
    stlxdm_server_cleanup(&server);
    
    // Clean up compositor
    stlxdm_compositor_cleanup(&compositor);

    // Clean up graphics context
    stlxgfx_cleanup(gfx_ctx);
    return 0;
}
