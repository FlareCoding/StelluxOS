#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include <stlibc/stlibc.h>
#include <stlxgfx/stlxgfx.h>

// Global variables for the application
static stlxgfx_context_t* g_ctx = NULL;
static stlxgfx_window_t* g_window = NULL;
static int g_quit_requested = 0;

// Terminal display variables
#define MAX_LINES 20
#define MAX_LINE_LENGTH 60
static char terminal_lines[MAX_LINES][MAX_LINE_LENGTH + 1];
static int current_line = 0;
static int current_column = 0;
static int cursor_blink_state = 0;
static uint32_t cursor_blink_timer = 0;
static int scroll_offset = 0; // Track how many lines we've scrolled

// Initialize terminal display
static void init_terminal_display() {
    memset(terminal_lines, 0, sizeof(terminal_lines));
    current_line = 0;
    current_column = 0;
    cursor_blink_state = 0;
    cursor_blink_timer = 0;
    scroll_offset = 0;
    
    // Add welcome message
    strcpy(terminal_lines[0], "Welcome to StelluxOS Shell!");
    strcpy(terminal_lines[1], "");
    strcpy(terminal_lines[2], "> ");
    current_line = 2;
    current_column = 2;
}

// Add a line to the terminal display
static void add_terminal_line(const char* text) {
    // If we're at the last line, scroll everything up
    if (current_line >= MAX_LINES - 1) {
        // Scroll all lines up
        for (int i = 0; i < MAX_LINES - 1; i++) {
            strcpy(terminal_lines[i], terminal_lines[i + 1]);
        }
        // Add new line at the bottom
        snprintf(terminal_lines[MAX_LINES - 1], MAX_LINE_LENGTH + 1, "%.*s", MAX_LINE_LENGTH, text);
        current_line = MAX_LINES - 1;
        scroll_offset++; // Track that we've scrolled
    } else {
        // Move to next line and add the text
        current_line++;
        snprintf(terminal_lines[current_line], MAX_LINE_LENGTH + 1, "%.*s", MAX_LINE_LENGTH, text);
    }
    
    // Reset cursor to prompt position on the new line
    current_column = 2; // After the "> " prompt
}

// Add a character to the current line
static void add_terminal_char(char c) {
    if (current_column < MAX_LINE_LENGTH - 1) {
        terminal_lines[current_line][current_column] = c;
        terminal_lines[current_line][current_column + 1] = '\0';
        current_column++;
    }
}

// Handle backspace
static void handle_backspace() {
    if (current_column > 2) { // Don't delete the "> " prompt
        current_column--;
        terminal_lines[current_line][current_column] = '\0';
    }
}

// Re-render the terminal display
static void render_terminal_display() {
    stlxgfx_surface_t* surface = stlxgfx_get_active_surface(g_window);
    if (!surface) {
        return;
    }
    
    int window_width = 460;
    int window_height = 340;
    
    // Clear surface with dark terminal background
    stlxgfx_clear_surface(surface, 0xFF0C0C0C);
    
    // Draw terminal border
    stlxgfx_draw_rect(surface, 10, 10, window_width - 20, window_height - 20, 0xFF404040);
    
    // Draw terminal background
    stlxgfx_fill_rect(surface, 12, 12, window_width - 24, window_height - 24, 0xFF1E1E1E);
    
    // Update cursor blink
    cursor_blink_timer++;
    if (cursor_blink_timer >= 30) { // Blink every 30 frames (~500ms at 60fps)
        cursor_blink_state = !cursor_blink_state;
        cursor_blink_timer = 0;
    }
    
    // Calculate visible area
    int text_x = 20;
    int text_y = 25;
    int line_height = 16;
    int max_visible_lines = (window_height - 60) / line_height; // ~17 lines visible
    
    // Calculate which lines to show (with scrolling)
    int start_line = 0;
    int end_line = MAX_LINES;
    
    // If we have more lines than can fit, show the most recent ones
    if (current_line >= max_visible_lines) {
        start_line = current_line - max_visible_lines + 1;
        end_line = current_line + 1;
    }
    
    // Draw visible lines
    for (int i = start_line; i < end_line && i < MAX_LINES; i++) {
        if (terminal_lines[i][0] != '\0') {
            int display_y = text_y + ((i - start_line) * line_height);
            stlxgfx_render_text(g_ctx, surface, terminal_lines[i], 
                               text_x, display_y, 14, 0xFFE0E0E0);
            
            // Draw blinking cursor on current line
            if (i == current_line && cursor_blink_state) {
                // Calculate cursor position using proper text metrics
                int cursor_x = text_x;
                if (current_column > 0) {
                    // Get the text up to the cursor position
                    char temp[MAX_LINE_LENGTH + 1];
                    strncpy(temp, terminal_lines[i], current_column);
                    temp[current_column] = '\0';
                    
                    // Measure the actual width of the text
                    stlxgfx_text_size_t text_size;
                    if (stlxgfx_get_text_size(g_ctx, temp, 14, &text_size) == 0) {
                        cursor_x += text_size.width;
                    } else {
                        // Fallback to character approximation if measurement fails
                        cursor_x += current_column * 9;
                    }
                }
                
                // Draw cursor with proper height and positioning
                int cursor_height = 14; // Match font size
                int cursor_y = display_y + 2; // Align with text baseline
                stlxgfx_fill_rect(surface, cursor_x, cursor_y, 2, cursor_height, 0xFFE0E0E0);
            }
        }
    }
    
    stlxgfx_swap_buffers(g_window);
}

// Event callback function
void handle_event(stlxgfx_window_t* window, const stlxgfx_event_t* event) {
    if (!window || !event) {
        return;
    }
    
    bool needs_redraw = false;
    
    switch (event->type) {
        case STLXGFX_KBD_EVT_KEY_PRESSED: {
            int ascii_char = event->sdata1;
            
            // Handle special keys
            if (event->udata1 == 0x2A) { // Backspace
                handle_backspace();
                needs_redraw = true;
            } else if (event->udata1 == 0x28) { // Enter
                // Execute the command (for now, just echo it)
                char command[MAX_LINE_LENGTH + 10];
                snprintf(command, sizeof(command), "Executed: %s", 
                        terminal_lines[current_line] + 2); // Skip "> "
                add_terminal_line(command);
                add_terminal_line("> ");
                needs_redraw = true;
            } else if (ascii_char >= 32 && ascii_char <= 126) {
                // Handle printable characters
                add_terminal_char((char)ascii_char);
                needs_redraw = true;
            }
            break;
        }
        
        case STLXGFX_KBD_EVT_KEY_RELEASED: {
            break;
        }
        
        case STLXGFX_POINTER_EVT_MOUSE_MOVED: {
            break;
        }
        
        case STLXGFX_POINTER_EVT_MOUSE_BTN_PRESSED: {
            printf("[SHELL] Mouse button pressed: button=%u at (%u, %d)\n", 
                   event->udata1, event->udata2, event->sdata1);
            break;
        }
        
        case STLXGFX_POINTER_EVT_MOUSE_BTN_RELEASED: {
            printf("[SHELL] Mouse button released: button=%u at (%u, %d)\n", 
                   event->udata1, event->udata2, event->sdata1);
            break;
        }
        
        case STLXGFX_POINTER_EVT_MOUSE_SCROLLED: {
            printf("[SHELL] Mouse scrolled: type=%u, delta=%d at (%u, %d)\n", 
                   event->udata1, event->sdata2,
                   event->udata2, event->sdata1);
            break;
        }
        
        default: {
            printf("[SHELL] Unknown event type: %u\n", event->type);
            break;
        }
    }
    
    // Re-render if needed
    if (needs_redraw) {
        render_terminal_display();
    }
}

int main() {
    // Wait for display manager to be ready
    struct timespec ts = { 5, 0 }; // 5 seconds
    nanosleep(&ts, NULL);
    
    // Initialize graphics library in application mode
    printf("[SHELL] Initializing graphics library in application mode...\n");
    g_ctx = stlxgfx_init(STLXGFX_MODE_APPLICATION);
    if (!g_ctx) {
        printf("[SHELL] ERROR: Failed to initialize graphics library\n");
        return 1;
    }
    
    // Set up event callback
    printf("[SHELL] Setting up event callback...\n");
    if (stlxgfx_set_event_callback(handle_event) != 0) {
        printf("[SHELL] WARNING: Failed to set event callback\n");
    }
    
    // Create a test window
    printf("[SHELL] Creating window (460x340) at position (200, 150) with title...\n");
    g_window = stlxgfx_create_window(g_ctx, 460, 340, 200, 150, "StelluxOS Terminal");
    if (!g_window) {
        printf("[SHELL] ERROR: Failed to create window\n");
        stlxgfx_cleanup(g_ctx);
        return 1;
    }
    
    // Initialize terminal display
    init_terminal_display();
    
    int window_width = 460;
    int window_height = 340;
    
    printf("[SHELL] Starting terminal interface with event handling...\n");
    
    // Initial render
    stlxgfx_surface_t* surface = stlxgfx_get_active_surface(g_window);
    if (surface) {
        // Clear surface with dark terminal background
        stlxgfx_clear_surface(surface, 0xFF0C0C0C);
        
        // Draw terminal border
        stlxgfx_draw_rect(surface, 10, 10, window_width - 20, window_height - 20, 0xFF404040);
        
        // Draw terminal background
        stlxgfx_fill_rect(surface, 12, 12, window_width - 24, window_height - 24, 0xFF1E1E1E);
        
        // Draw initial terminal text (clipped to terminal area)
        int text_x = 20;
        int text_y = 25;
        int line_height = 16;
        int max_visible_lines = (window_height - 60) / line_height; // ~17 lines visible
        
        // Calculate which lines to show (with scrolling)
        int start_line = 0;
        int end_line = MAX_LINES;
        
        // If we have more lines than can fit, show the most recent ones
        if (current_line >= max_visible_lines) {
            start_line = current_line - max_visible_lines + 1;
            end_line = current_line + 1;
        }
        
        // Draw visible lines
        for (int i = start_line; i < end_line && i < MAX_LINES; i++) {
            if (terminal_lines[i][0] != '\0') {
                int display_y = text_y + ((i - start_line) * line_height);
                stlxgfx_render_text(g_ctx, surface, terminal_lines[i], 
                                   text_x, display_y, 14, 0xFFE0E0E0);
            }
        }
        
        stlxgfx_swap_buffers(g_window);
    }
    
    // Keep the window "alive" with event processing only
    while (!g_quit_requested) {
        // Poll for events (this will call our callback if events are available)
        stlxgfx_poll_events();
        
        // Small delay to avoid busy waiting
        struct timespec frame_delay = { 0, 16 * 1000 * 1000 }; // 16ms
        nanosleep(&frame_delay, NULL);
    }
    
    // Clean up
    printf("[SHELL] Quit requested - cleaning up window and graphics context...\n");
    stlxgfx_destroy_window(g_ctx, g_window);
    stlxgfx_cleanup(g_ctx);
    
    printf("[SHELL] Terminal interface completed successfully!\n");
    return 0;
}
