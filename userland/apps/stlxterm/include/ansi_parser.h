#ifndef STLXTERM_ANSI_PARSER_H
#define STLXTERM_ANSI_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include "terminal.h"

// ANSI escape sequence types
typedef enum {
    ANSI_NONE,
    ANSI_CSI,        // Control Sequence Introducer
    ANSI_OSC,        // Operating System Command
    ANSI_ESC,        // Escape sequence
    ANSI_DCS,        // Device Control String
    ANSI_PM,         // Privacy Message
    ANSI_APC,        // Application Program Command
    ANSI_SOS,        // Start of String
    ANSI_PM_APC      // Privacy Message or Application Program Command
} ansi_sequence_type_t;

// ANSI parser state
typedef struct {
    ansi_sequence_type_t type;
    char buffer[256];
    int buffer_pos;
    bool in_sequence;
    bool intermediate;
    char intermediate_char;
    int params[16];
    int param_count;
    char final_char;
} ansi_parser_t;

// Function prototypes
void ansi_parser_init(ansi_parser_t* parser);
void ansi_parser_reset(ansi_parser_t* parser);
void ansi_parser_process_char(ansi_parser_t* parser, terminal_t* term, char c);
void ansi_parser_execute_sequence(ansi_parser_t* parser, terminal_t* term);

// ANSI sequence handlers
void ansi_handle_cursor_position(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_cursor_up(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_cursor_down(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_cursor_forward(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_cursor_backward(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_cursor_save(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_cursor_restore(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_clear_screen(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_clear_line(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_set_graphics_mode(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_reset_graphics_mode(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_scroll_up(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_scroll_down(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_set_mode(ansi_parser_t* parser, terminal_t* term);
void ansi_handle_reset_mode(ansi_parser_t* parser, terminal_t* term);

// Color handling
uint32_t ansi_color_to_rgb(int color_code, bool bright);
void ansi_apply_color_attributes(terminal_t* term, int* params, int count);

#endif // STLXTERM_ANSI_PARSER_H
