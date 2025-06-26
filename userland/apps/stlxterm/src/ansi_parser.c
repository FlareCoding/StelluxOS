#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ansi_parser.h"

// Initialize ANSI parser
void ansi_parser_init(ansi_parser_t* parser) {
    if (!parser) {
        return;
    }
    
    memset(parser, 0, sizeof(ansi_parser_t));
    parser->type = ANSI_NONE;
    parser->buffer_pos = 0;
    parser->in_sequence = false;
    parser->intermediate = false;
    parser->param_count = 0;
}

// Reset ANSI parser
void ansi_parser_reset(ansi_parser_t* parser) {
    if (!parser) {
        return;
    }
    
    parser->type = ANSI_NONE;
    parser->buffer_pos = 0;
    parser->in_sequence = false;
    parser->intermediate = false;
    parser->intermediate_char = 0;
    parser->param_count = 0;
    parser->final_char = 0;
    memset(parser->buffer, 0, sizeof(parser->buffer));
    memset(parser->params, 0, sizeof(parser->params));
}

// Process a character through the ANSI parser
void ansi_parser_process_char(ansi_parser_t* parser, terminal_t* term, char c) {
    if (!parser || !term) {
        return;
    }
    
    // If we're not in a sequence and this is an escape character
    if (!parser->in_sequence && c == 0x1B) {
        parser->in_sequence = true;
        parser->buffer_pos = 0;
        parser->buffer[parser->buffer_pos++] = c;
        return;
    }
    
    // If we're in a sequence, collect characters
    if (parser->in_sequence) {
        if (parser->buffer_pos < (int)(sizeof(parser->buffer) - 1)) {
            parser->buffer[parser->buffer_pos++] = c;
        }
        
        // Check for sequence end
        if (c >= 0x40 && c <= 0x7E) {
            // Final character found, execute the sequence
            parser->final_char = c;
            ansi_parser_execute_sequence(parser, term);
            ansi_parser_reset(parser);
        }
    } else {
        // Not in a sequence, just write the character normally
        terminal_write_char(term, c);
    }
}

// Execute an ANSI escape sequence
void ansi_parser_execute_sequence(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) {
        return;
    }
    
    // For now, just ignore ANSI sequences
    // This will be implemented later with full ANSI support
    printf("[ANSI] Ignoring sequence: %s\n", parser->buffer);
}

// Stub implementations for ANSI sequence handlers
void ansi_handle_cursor_position(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_cursor_up(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_cursor_down(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_cursor_forward(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_cursor_backward(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_cursor_save(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    terminal_save_cursor(term);
}

void ansi_handle_cursor_restore(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    terminal_restore_cursor(term);
}

void ansi_handle_clear_screen(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    terminal_clear_screen(term);
}

void ansi_handle_clear_line(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_set_graphics_mode(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_reset_graphics_mode(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    terminal_reset_colors(term);
}

void ansi_handle_scroll_up(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_scroll_down(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_set_mode(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

void ansi_handle_reset_mode(ansi_parser_t* parser, terminal_t* term) {
    if (!parser || !term) return;
    // Implementation will be added later
}

// Convert ANSI color code to RGB
uint32_t ansi_color_to_rgb(int color_code, bool bright) {
    // Basic 8-color ANSI palette
    static const uint32_t colors[8] = {
        0xFF000000, // Black
        0xFF800000, // Red
        0xFF008000, // Green
        0xFF808000, // Yellow
        0xFF000080, // Blue
        0xFF800080, // Magenta
        0xFF008080, // Cyan
        0xFFC0C0C0  // White
    };
    
    if (color_code >= 0 && color_code < 8) {
        uint32_t color = colors[color_code];
        if (bright && color_code != 0) { // Don't brighten black
            // Make the color brighter
            int r = (color >> 16) & 0xFF;
            int g = (color >> 8) & 0xFF;
            int b = color & 0xFF;
            
            r = (r * 3) / 2; if (r > 255) r = 255;
            g = (g * 3) / 2; if (g > 255) g = 255;
            b = (b * 3) / 2; if (b > 255) b = 255;
            
            return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        return color;
    }
    
    return 0xFFE0E0E0; // Default to light gray
}

// Apply color attributes to terminal
void ansi_apply_color_attributes(terminal_t* term, int* params, int count) {
    if (!term || !params || count <= 0) {
        return;
    }
    
    for (int i = 0; i < count; i++) {
        int code = params[i];
        
        switch (code) {
            case 0: // Reset
                terminal_reset_colors(term);
                break;
            case 1: // Bold
                // Implementation will be added later
                break;
            case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
                // Foreground colors
                terminal_set_foreground_color(term, ansi_color_to_rgb(code - 30, false));
                break;
            case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
                // Background colors
                terminal_set_background_color(term, ansi_color_to_rgb(code - 40, false));
                break;
            default:
                // Ignore unknown codes
                break;
        }
    }
}
