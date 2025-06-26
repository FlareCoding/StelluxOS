#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <stlibc/stlibc.h>
#include <stlxgfx/stlxgfx.h>

#include "terminal.h"
#include "ansi_parser.h"
#include "process_manager.h"

// Global terminal instance
static terminal_t* g_terminal = NULL;

// Event callback function for graphics library
void handle_event(stlxgfx_window_t* window, const stlxgfx_event_t* event) {
    if (!g_terminal || !window || !event) {
        return;
    }
    
    // Pass the event to our terminal handler
    terminal_handle_event(g_terminal, event);
}

int main() {
    printf("[STLXTERM] Starting Stellux Terminal Emulator...\n");
    
    // Create terminal with smaller size (72x18 characters)
    int window_width = 576;  // 72 chars * 8 pixels + margins
    int window_height = 360; // 18 chars * 16 pixels + margins
    int cols = TERMINAL_DEFAULT_COLS;
    int rows = TERMINAL_DEFAULT_ROWS;
    
    printf("[STLXTERM] Creating terminal with %dx%d characters (%dx%d pixels)...\n", 
           cols, rows, window_width, window_height);
    
    g_terminal = terminal_create(cols, rows, window_width, window_height);
    if (!g_terminal) {
        printf("[STLXTERM] ERROR: Failed to create terminal\n");
        return 1;
    }
    
    // Initialize the terminal
    printf("[STLXTERM] Initializing terminal...\n");
    if (terminal_init(g_terminal) != 0) {
        printf("[STLXTERM] ERROR: Failed to initialize terminal\n");
        terminal_destroy(g_terminal);
        return 1;
    }
    
    // Set up event callback
    printf("[STLXTERM] Setting up event callback...\n");
    if (stlxgfx_set_event_callback(handle_event) != 0) {
        printf("[STLXTERM] WARNING: Failed to set event callback\n");
    }
    
    // Display welcome message
    terminal_write_string(g_terminal, "Welcome to Stellux Terminal Emulator!\r\n");
    terminal_write_string(g_terminal, "Terminal is ready for input.\r\n");
    terminal_write_string(g_terminal, "$ ");
    
    printf("[STLXTERM] Terminal ready - starting main loop...\n");
    
    // Main terminal loop
    terminal_main_loop(g_terminal);
    
    // Cleanup
    printf("[STLXTERM] Shutting down terminal...\n");
    terminal_cleanup(g_terminal);
    terminal_destroy(g_terminal);
    
    printf("[STLXTERM] Terminal emulator completed successfully!\n");
    return 0;
}
