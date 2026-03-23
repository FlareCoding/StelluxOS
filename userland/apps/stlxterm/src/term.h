#ifndef STLXTERM_TERM_H
#define STLXTERM_TERM_H

#include <stdint.h>

#define TERM_MAX_COLS 120
#define TERM_MAX_ROWS 40
#define TERM_CSI_MAX  32
#define TERM_CSI_MAX_PARAMS 8

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
    uint32_t fg;
    uint32_t bg;
    char     ch;
    char     _pad[3];
} term_cell;

typedef struct {
    term_cell cells[TERM_MAX_ROWS][TERM_MAX_COLS];
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
} term_state;

void term_init(term_state* t, int rows, int cols,
               uint32_t default_fg, uint32_t default_bg);

void term_feed(term_state* t, const char* data, int len);

#endif /* STLXTERM_TERM_H */
