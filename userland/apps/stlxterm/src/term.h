#ifndef STLXTERM_TERM_H
#define STLXTERM_TERM_H

#include <stdint.h>

#define TERM_MAX_COLS    200
#define TERM_MAX_ROWS    80
#define TERM_CSI_MAX_PARAMS 8

typedef struct {
    uint8_t fg; // 0 = default, 1-8 = ANSI 30-37, 9-16 = bright 90-97
    uint8_t bg; // 0 = default, 1-8 = ANSI 40-47, 9-16 = bright 100-107
} term_attr_t;

typedef struct {
    char cells[TERM_MAX_ROWS][TERM_MAX_COLS];
    term_attr_t attrs[TERM_MAX_ROWS][TERM_MAX_COLS];
    int rows;
    int cols;
    int cursor_row;
    int cursor_col;
    int dirty;

    uint8_t cur_fg;
    uint8_t cur_bg;
    uint8_t cur_bold;

    enum { TERM_ST_NORMAL, TERM_ST_ESC, TERM_ST_CSI } state;
    int csi_params[TERM_CSI_MAX_PARAMS];
    int csi_param_count;
    int csi_current_param;
} term_state_t;

void term_init(term_state_t *t, int rows, int cols);
void term_feed(term_state_t *t, const char *data, int len);

#endif // STLXTERM_TERM_H
