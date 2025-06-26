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

// Default colors
#define DEFAULT_FG_COLOR 0xFFE0E0E0  // Light gray
#define DEFAULT_BG_COLOR 0xFF1E1E1E  // Dark gray
#define CURSOR_COLOR 0xFFE0E0E0       // White cursor

// Create a new terminal instance
terminal_t* terminal_create(int cols, int rows, int window_width, int window_height) {
    terminal_t* term = malloc(sizeof(terminal_t));
    if (!term) {
        return NULL;
    }
    
    // Initialize all fields to zero
    memset(term, 0, sizeof(terminal_t));
    
    // Set terminal dimensions
    term->state.cols = cols;
    term->state.rows = rows;
    term->window_width = window_width;
    term->window_height = window_height;
    
    // Set character dimensions (approximate)
    term->char_width = 8;
    term->char_height = 16;
    
    // Calculate margins to center the terminal
    term->margin_x = (window_width - (cols * term->char_width)) / 2;
    term->margin_y = (window_height - (rows * term->char_height)) / 2;
    
    // Initialize terminal state
    terminal_reset(term);
    
    return term;
}

// Destroy terminal instance
void terminal_destroy(terminal_t* term) {
    if (!term) {
        return;
    }
    
    terminal_cleanup(term);
    free(term);
}

// Initialize terminal (create window, etc.)
int terminal_init(terminal_t* term) {
    if (!term) {
        return -1;
    }
    
    // Initialize graphics library in application mode
    printf("[TERMINAL] Initializing graphics library...\n");
    term->gfx_ctx = stlxgfx_init(STLXGFX_MODE_APPLICATION);
    if (!term->gfx_ctx) {
        printf("[TERMINAL] ERROR: Failed to initialize graphics library\n");
        return -1;
    }
    
    // Create window
    printf("[TERMINAL] Creating window (%dx%d)...\n", term->window_width, term->window_height);
    term->window = stlxgfx_create_window(term->gfx_ctx, 
                                        term->window_width, term->window_height, 
                                        40, 40, "StelluxOS Terminal");
    if (!term->window) {
        printf("[TERMINAL] ERROR: Failed to create window\n");
        stlxgfx_cleanup(term->gfx_ctx);
        return -1;
    }
    
    // Set running state
    term->running = true;
    term->needs_redraw = true;
    
    printf("[TERMINAL] Terminal initialized successfully\n");
    return 0;
}

// Cleanup terminal resources
void terminal_cleanup(terminal_t* term) {
    if (!term) {
        return;
    }
    
    if (term->window) {
        stlxgfx_destroy_window(term->gfx_ctx, term->window);
        term->window = NULL;
    }
    
    if (term->gfx_ctx) {
        stlxgfx_cleanup(term->gfx_ctx);
        term->gfx_ctx = NULL;
    }
    
    term->running = false;
}

// Reset terminal to initial state
void terminal_reset(terminal_t* term) {
    if (!term) {
        return;
    }
    
    // Reset cursor
    term->state.cursor_x = 0;
    term->state.cursor_y = 0;
    term->state.saved_cursor_x = 0;
    term->state.saved_cursor_y = 0;
    term->state.cursor_visible = true;
    term->state.cursor_blink = true;
    term->state.cursor_blink_timer = 0;
    
    // Reset colors
    term->state.default_fg_color = DEFAULT_FG_COLOR;
    term->state.default_bg_color = DEFAULT_BG_COLOR;
    term->state.current_fg_color = DEFAULT_FG_COLOR;
    term->state.current_bg_color = DEFAULT_BG_COLOR;
    
    // Reset attributes
    term->state.bold = false;
    term->state.italic = false;
    term->state.underline = false;
    term->state.reverse = false;
    
    // Reset scrolling
    term->state.scroll_top = 0;
    term->state.scroll_bottom = term->state.rows - 1;
    
    // Clear grid
    for (int y = 0; y < TERMINAL_MAX_ROWS; y++) {
        for (int x = 0; x < TERMINAL_MAX_COLS; x++) {
            term->state.grid[y][x].character = ' ';
            term->state.grid[y][x].foreground_color = DEFAULT_FG_COLOR;
            term->state.grid[y][x].background_color = DEFAULT_BG_COLOR;
            term->state.grid[y][x].bold = false;
            term->state.grid[y][x].italic = false;
            term->state.grid[y][x].underline = false;
            term->state.grid[y][x].reverse = false;
        }
    }
    
    // Reset selection
    term->state.selection_active = false;
    
    // Reset buffers
    term->input_buffer_pos = 0;
    term->output_buffer_pos = 0;
}

// Main terminal loop
void terminal_main_loop(terminal_t* term) {
    if (!term || !term->running) {
        return;
    }
    
    printf("[TERMINAL] Starting main loop...\n");
    
    while (term->running && stlxgfx_is_window_opened(term->window)) {
        // Poll for events
        stlxgfx_poll_events();
        
        // Update cursor blink
        term->state.cursor_blink_timer++;
        if (term->state.cursor_blink_timer >= 30) { // Blink every 30 frames
            term->state.cursor_blink = !term->state.cursor_blink;
            term->state.cursor_blink_timer = 0;
            term->needs_redraw = true;
        }
        
        // Render if needed
        if (term->needs_redraw) {
            terminal_render(term);
            term->needs_redraw = false;
        }
        
        // Small delay to avoid busy waiting
        struct timespec frame_delay = { 0, 16 * 1000 * 1000 }; // 16ms
        nanosleep(&frame_delay, NULL);
    }
    
    printf("[TERMINAL] Main loop ended\n");
}

// Render the terminal
void terminal_render(terminal_t* term) {
    if (!term || !term->window) {
        return;
    }
    
    stlxgfx_surface_t* surface = stlxgfx_get_active_surface(term->window);
    if (!surface) {
        return;
    }
    
    // Clear surface with terminal background color
    stlxgfx_clear_surface(surface, term->state.default_bg_color);
    
    // Draw terminal border
    stlxgfx_draw_rect(surface, 5, 5, term->window_width - 10, term->window_height - 10, 0xFF404040);
    
    // Draw character grid
    for (int y = 0; y < term->state.rows; y++) {
        for (int x = 0; x < term->state.cols; x++) {
            terminal_cell_t* cell = &term->state.grid[y][x];
            
            // Calculate screen position
            int screen_x = term->margin_x + (x * term->char_width);
            int screen_y = term->margin_y + (y * term->char_height);
            
            // Draw character background
            stlxgfx_fill_rect(surface, screen_x, screen_y, 
                             term->char_width, term->char_height, 
                             cell->background_color);
            
            // Draw character
            if (cell->character != ' ') {
                char char_str[2] = { cell->character, '\0' };
                stlxgfx_render_text(term->gfx_ctx, surface, char_str, 
                                   screen_x, screen_y, 14, cell->foreground_color);
            }
        }
    }
    
    // Draw cursor if visible and blinking
    if (term->state.cursor_visible && term->state.cursor_blink) {
        int cursor_x = term->margin_x + (term->state.cursor_x * term->char_width);
        int cursor_y = term->margin_y + (term->state.cursor_y * term->char_height);
        
        stlxgfx_fill_rect(surface, cursor_x, cursor_y, 2, term->char_height, CURSOR_COLOR);
    }
    
    // Swap buffers
    stlxgfx_swap_buffers(term->window);
}

// Handle events
void terminal_handle_event(terminal_t* term, const stlxgfx_event_t* event) {
    if (!term || !event) {
        return;
    }
    
    bool needs_redraw = false;
    
    switch (event->type) {
        case STLXGFX_KBD_EVT_KEY_PRESSED: {
            int ascii_char = event->sdata1;
            
            // Handle special keys
            if (event->udata1 == 0x2A) { // Backspace
                if (term->state.cursor_x > 0) {
                    term->state.cursor_x--;
                    terminal_write_char(term, ' '); // Clear the character
                    term->state.cursor_x--; // Move back again
                }
                needs_redraw = true;
            } else if (event->udata1 == 0x28) { // Enter
                terminal_write_char(term, '\r');
                terminal_write_char(term, '\n');
                needs_redraw = true;
            } else if (ascii_char >= 32 && ascii_char <= 126) {
                // Handle printable characters
                terminal_write_char(term, (char)ascii_char);
                needs_redraw = true;
            }
            break;
        }
        
        case STLXGFX_KBD_EVT_KEY_RELEASED: {
            break;
        }
        
        default: {
            // Ignore other events for now
            break;
        }
    }
    
    if (needs_redraw) {
        term->needs_redraw = true;
    }
}

// Write a character to the terminal
void terminal_write_char(terminal_t* term, char c) {
    if (!term) {
        return;
    }
    
    switch (c) {
        case '\r': // Carriage return
            term->state.cursor_x = 0;
            break;
            
        case '\n': // Line feed
            term->state.cursor_y++;
            if (term->state.cursor_y >= term->state.rows) {
                terminal_scroll_up(term, 1);
                term->state.cursor_y = term->state.rows - 1;
            }
            break;
            
        case '\t': // Tab
            term->state.cursor_x = (term->state.cursor_x + 8) & ~7; // Align to 8
            if (term->state.cursor_x >= term->state.cols) {
                term->state.cursor_x = 0;
                term->state.cursor_y++;
                if (term->state.cursor_y >= term->state.rows) {
                    terminal_scroll_up(term, 1);
                    term->state.cursor_y = term->state.rows - 1;
                }
            }
            break;
            
        default: // Regular character
            if (term->state.cursor_x < term->state.cols && term->state.cursor_y < term->state.rows) {
                terminal_cell_t* cell = &term->state.grid[term->state.cursor_y][term->state.cursor_x];
                cell->character = c;
                cell->foreground_color = term->state.current_fg_color;
                cell->background_color = term->state.current_bg_color;
                cell->bold = term->state.bold;
                cell->italic = term->state.italic;
                cell->underline = term->state.underline;
                cell->reverse = term->state.reverse;
                
                term->state.cursor_x++;
                if (term->state.cursor_x >= term->state.cols) {
                    term->state.cursor_x = 0;
                    term->state.cursor_y++;
                    if (term->state.cursor_y >= term->state.rows) {
                        terminal_scroll_up(term, 1);
                        term->state.cursor_y = term->state.rows - 1;
                    }
                }
            }
            break;
    }
}

// Write a string to the terminal
void terminal_write_string(terminal_t* term, const char* str) {
    if (!term || !str) {
        return;
    }
    
    for (int i = 0; str[i] != '\0'; i++) {
        terminal_write_char(term, str[i]);
    }
}

// Scroll the terminal up
void terminal_scroll_up(terminal_t* term, int lines) {
    if (!term || lines <= 0) {
        return;
    }
    
    // Move all lines up
    for (int y = 0; y < term->state.rows - lines; y++) {
        for (int x = 0; x < term->state.cols; x++) {
            term->state.grid[y][x] = term->state.grid[y + lines][x];
        }
    }
    
    // Clear the bottom lines
    for (int y = term->state.rows - lines; y < term->state.rows; y++) {
        for (int x = 0; x < term->state.cols; x++) {
            term->state.grid[y][x].character = ' ';
            term->state.grid[y][x].foreground_color = term->state.default_fg_color;
            term->state.grid[y][x].background_color = term->state.default_bg_color;
            term->state.grid[y][x].bold = false;
            term->state.grid[y][x].italic = false;
            term->state.grid[y][x].underline = false;
            term->state.grid[y][x].reverse = false;
        }
    }
}

// Stub implementations for other functions
void terminal_clear_screen(terminal_t* term) {
    if (!term) return;
    terminal_reset(term);
}

void terminal_clear_line(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->state.rows) return;
    // Implementation will be added later
}

void terminal_scroll_down(terminal_t* term, int lines) {
    if (!term || lines <= 0) return;
    // Implementation will be added later
}

void terminal_set_cursor(terminal_t* term, int x, int y) {
    if (!term) return;
    term->state.cursor_x = x;
    term->state.cursor_y = y;
}

void terminal_move_cursor(terminal_t* term, int dx, int dy) {
    if (!term) return;
    terminal_set_cursor(term, term->state.cursor_x + dx, term->state.cursor_y + dy);
}

void terminal_show_cursor(terminal_t* term, bool show) {
    if (!term) return;
    term->state.cursor_visible = show;
}

void terminal_save_cursor(terminal_t* term) {
    if (!term) return;
    term->state.saved_cursor_x = term->state.cursor_x;
    term->state.saved_cursor_y = term->state.cursor_y;
}

void terminal_restore_cursor(terminal_t* term) {
    if (!term) return;
    term->state.cursor_x = term->state.saved_cursor_x;
    term->state.cursor_y = term->state.saved_cursor_y;
}

void terminal_insert_char(terminal_t* term, char c) {
    (void)term; // Mark as unused
    (void)c;    // Mark as unused
    // Implementation will be added later
}

void terminal_delete_char(terminal_t* term) {
    if (!term) return;
    // Implementation will be added later
}

void terminal_set_foreground_color(terminal_t* term, uint32_t color) {
    if (!term) return;
    term->state.current_fg_color = color;
}

void terminal_set_background_color(terminal_t* term, uint32_t color) {
    if (!term) return;
    term->state.current_bg_color = color;
}

void terminal_reset_colors(terminal_t* term) {
    if (!term) return;
    term->state.current_fg_color = term->state.default_fg_color;
    term->state.current_bg_color = term->state.default_bg_color;
}

void terminal_process_input(terminal_t* term) {
    if (!term) return;
    // Implementation will be added later
}

void terminal_resize(terminal_t* term, int cols, int rows) {
    (void)term; // Mark as unused
    (void)cols; // Mark as unused
    (void)rows; // Mark as unused
    // Implementation will be added later
}

void terminal_bell(terminal_t* term) {
    if (!term) return;
    // Implementation will be added later
}
