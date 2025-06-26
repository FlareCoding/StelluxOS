#ifndef STLXTERM_TERMINAL_H
#define STLXTERM_TERMINAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stlxgfx/stlxgfx.h>

// Terminal configuration
#define TERMINAL_MAX_COLS 120
#define TERMINAL_MAX_ROWS 40
#define TERMINAL_DEFAULT_COLS 80
#define TERMINAL_DEFAULT_ROWS 24

// Terminal cell structure
typedef struct {
    char character;
    uint32_t foreground_color;
    uint32_t background_color;
    bool bold;
    bool italic;
    bool underline;
    bool reverse;
} terminal_cell_t;

// Terminal state
typedef struct {
    int cursor_x;
    int cursor_y;
    int saved_cursor_x;
    int saved_cursor_y;
    bool cursor_visible;
    bool cursor_blink;
    uint32_t cursor_blink_timer;
    
    // Colors
    uint32_t default_fg_color;
    uint32_t default_bg_color;
    uint32_t current_fg_color;
    uint32_t current_bg_color;
    
    // Attributes
    bool bold;
    bool italic;
    bool underline;
    bool reverse;
    
    // Scrolling
    int scroll_top;
    int scroll_bottom;
    
    // Terminal size
    int cols;
    int rows;
    
    // Character grid
    terminal_cell_t grid[TERMINAL_MAX_ROWS][TERMINAL_MAX_COLS];
    
    // Selection
    bool selection_active;
    int selection_start_x;
    int selection_start_y;
    int selection_end_x;
    int selection_end_y;
} terminal_state_t;

// Terminal emulator structure
typedef struct {
    stlxgfx_context_t* gfx_ctx;
    stlxgfx_window_t* window;
    terminal_state_t state;
    
    // Rendering
    int char_width;
    int char_height;
    int window_width;
    int window_height;
    int margin_x;
    int margin_y;
    
    // Event handling
    bool running;
    bool needs_redraw;
    
    // Input buffer
    char input_buffer[1024];
    int input_buffer_pos;
    
    // Output buffer (for child process communication)
    char output_buffer[4096];
    int output_buffer_pos;
    
    // Command line handling
    char command_line[1024];
    int command_line_pos;
    bool command_ready;
} terminal_t;

// Function prototypes
terminal_t* terminal_create(int cols, int rows, int window_width, int window_height);
void terminal_destroy(terminal_t* term);
int terminal_init(terminal_t* term);
void terminal_cleanup(terminal_t* term);

// Rendering functions
void terminal_render(terminal_t* term);
void terminal_clear_screen(terminal_t* term);
void terminal_clear_line(terminal_t* term, int row);
void terminal_scroll_up(terminal_t* term, int lines);
void terminal_scroll_down(terminal_t* term, int lines);

// Cursor functions
void terminal_set_cursor(terminal_t* term, int x, int y);
void terminal_move_cursor(terminal_t* term, int dx, int dy);
void terminal_show_cursor(terminal_t* term, bool show);
void terminal_save_cursor(terminal_t* term);
void terminal_restore_cursor(terminal_t* term);

// Text functions
void terminal_write_char(terminal_t* term, char c);
void terminal_write_string(terminal_t* term, const char* str);
void terminal_insert_char(terminal_t* term, char c);
void terminal_delete_char(terminal_t* term);

// Color functions
void terminal_set_foreground_color(terminal_t* term, uint32_t color);
void terminal_set_background_color(terminal_t* term, uint32_t color);
void terminal_reset_colors(terminal_t* term);

// Event handling
void terminal_handle_event(terminal_t* term, const stlxgfx_event_t* event);
void terminal_process_input(terminal_t* term);
void terminal_main_loop(terminal_t* term);

// Utility functions
void terminal_resize(terminal_t* term, int cols, int rows);
void terminal_reset(terminal_t* term);
void terminal_bell(terminal_t* term);

#endif // STLXTERM_TERMINAL_H
