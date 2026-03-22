#ifndef STLXTERM_TERM_H
#define STLXTERM_TERM_H

#include <stdint.h>

#define TERM_MAX_COLS 200
#define TERM_MAX_ROWS 80

typedef enum {
    TERM_PARSE_NORMAL,
    TERM_PARSE_ESC,
    TERM_PARSE_CSI,
} term_parse_state;

typedef struct {
    char cells[TERM_MAX_ROWS][TERM_MAX_COLS];
    int rows;
    int cols;
    int cursor_row;
    int cursor_col;
    int dirty;

    term_parse_state parse_state;
    char csi_buf[32];
    int csi_len;
} term_state;

/**
 * Initialize terminal state with given grid dimensions.
 * Clears all cells to spaces and places cursor at (0,0).
 */
void term_init(term_state* t, int rows, int cols);

/**
 * Feed a buffer of bytes from the PTY master into the terminal.
 * Updates the screen buffer and cursor position. Sets t->dirty = 1
 * if any visible state changed.
 */
void term_feed(term_state* t, const char* data, int len);

#endif /* STLXTERM_TERM_H */
