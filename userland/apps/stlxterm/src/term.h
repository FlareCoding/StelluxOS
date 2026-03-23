#ifndef STLXTERM_TERM_H
#define STLXTERM_TERM_H

#include <stdint.h>

#define TERM_MAX_COLS 120
#define TERM_MAX_ROWS 40
#define TERM_CSI_MAX  32
#define TERM_CSI_MAX_PARAMS 8
#define TERM_SCROLLBACK_LINES 200

typedef enum {
    TERM_PARSE_NORMAL,
    TERM_PARSE_ESC,
    TERM_PARSE_CSI,
} term_parse_state;

typedef struct {
    uint32_t fg;
    uint32_t bg;
    uint8_t  bold;
} term_attr;

typedef struct {
    char     ch[TERM_MAX_COLS];
    uint32_t fg[TERM_MAX_COLS];
    uint32_t bg[TERM_MAX_COLS];
} term_line;

typedef struct {
    term_line lines[TERM_SCROLLBACK_LINES];
    int head;      /* next write position */
    int count;     /* number of stored lines (0..TERM_SCROLLBACK_LINES) */
} term_scrollback;

typedef struct {
    char     chars[TERM_MAX_ROWS][TERM_MAX_COLS];
    uint32_t fg[TERM_MAX_ROWS][TERM_MAX_COLS];
    uint32_t bg[TERM_MAX_ROWS][TERM_MAX_COLS];
    int rows;
    int cols;
    int cursor_row;
    int cursor_col;
    int dirty;

    term_attr current_attr;
    uint32_t default_fg;
    uint32_t default_bg;

    term_parse_state parse_state;
    char csi_buf[TERM_CSI_MAX];
    int csi_len;

    term_scrollback* scrollback;
    int scroll_offset; /* 0 = live view, >0 = scrolled back N lines */
} term_state;

void term_init(term_state* t, int rows, int cols,
               uint32_t default_fg, uint32_t default_bg);

void term_feed(term_state* t, const char* data, int len);

void term_scroll_up_view(term_state* t, int lines);
void term_scroll_down_view(term_state* t, int lines);
void term_scroll_to_bottom(term_state* t);

/**
 * Get the line to display at a given screen row, accounting for
 * scroll_offset. Returns pointers to the character, fg, and bg arrays.
 * Returns 0 if the row comes from scrollback, 1 if from the live buffer.
 */
int term_get_display_line(term_state* t, int screen_row,
                          const char** out_ch,
                          const uint32_t** out_fg,
                          const uint32_t** out_bg);

#endif /* STLXTERM_TERM_H */
