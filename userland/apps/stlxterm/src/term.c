#include "term.h"
#include <string.h>

void term_init(term_state* t, int rows, int cols) {
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    t->rows = rows;
    t->cols = cols;
    t->cursor_row = 0;
    t->cursor_col = 0;
    t->dirty = 1;
    t->parse_state = TERM_PARSE_NORMAL;
    t->csi_len = 0;
    memset(t->cells, ' ', sizeof(t->cells));
}

static void scroll_up(term_state* t) {
    for (int r = 0; r < t->rows - 1; r++) {
        memcpy(t->cells[r], t->cells[r + 1], (size_t)t->cols);
    }
    memset(t->cells[t->rows - 1], ' ', (size_t)t->cols);
}

static void advance_cursor(term_state* t) {
    t->cursor_col++;
    if (t->cursor_col >= t->cols) {
        t->cursor_col = 0;
        t->cursor_row++;
        if (t->cursor_row >= t->rows) {
            scroll_up(t);
            t->cursor_row = t->rows - 1;
        }
    }
}

static void newline(term_state* t) {
    t->cursor_col = 0;
    t->cursor_row++;
    if (t->cursor_row >= t->rows) {
        scroll_up(t);
        t->cursor_row = t->rows - 1;
    }
}

static int parse_csi_param(const char* buf, int len, int default_val) {
    if (len == 0) return default_val;
    int val = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            val = val * 10 + (buf[i] - '0');
        }
    }
    return val > 0 ? val : default_val;
}

static void handle_csi(term_state* t, char final) {
    int param = parse_csi_param(t->csi_buf, t->csi_len, 1);

    switch (final) {
    case 'A': /* Cursor Up */
        t->cursor_row -= param;
        if (t->cursor_row < 0) t->cursor_row = 0;
        break;
    case 'B': /* Cursor Down */
        t->cursor_row += param;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        break;
    case 'C': /* Cursor Forward */
        t->cursor_col += param;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;
    case 'D': /* Cursor Back */
        t->cursor_col -= param;
        if (t->cursor_col < 0) t->cursor_col = 0;
        break;
    case 'H': /* Cursor Home */
        t->cursor_row = 0;
        t->cursor_col = 0;
        break;
    case 'F': /* Cursor to end of line (End key response) */
        t->cursor_col = t->cols - 1;
        break;
    case 'J': { /* Erase in Display */
        int mode = parse_csi_param(t->csi_buf, t->csi_len, 0);
        if (mode == 2) {
            memset(t->cells, ' ', sizeof(t->cells));
            t->cursor_row = 0;
            t->cursor_col = 0;
        }
        break;
    }
    case 'K': { /* Erase in Line */
        int mode = parse_csi_param(t->csi_buf, t->csi_len, 0);
        if (mode == 0) {
            for (int c = t->cursor_col; c < t->cols; c++)
                t->cells[t->cursor_row][c] = ' ';
        } else if (mode == 1) {
            for (int c = 0; c <= t->cursor_col; c++)
                t->cells[t->cursor_row][c] = ' ';
        } else if (mode == 2) {
            memset(t->cells[t->cursor_row], ' ', (size_t)t->cols);
        }
        break;
    }
    case '~': { /* Special keys (Delete, PgUp, PgDn, etc.) -- ignored */
        break;
    }
    }
}

static void feed_byte(term_state* t, char c) {
    switch (t->parse_state) {
    case TERM_PARSE_NORMAL:
        if (c == '\x1b') {
            t->parse_state = TERM_PARSE_ESC;
        } else if (c == '\r') {
            t->cursor_col = 0;
        } else if (c == '\n') {
            newline(t);
        } else if (c == '\b' || c == '\x7f') {
            if (t->cursor_col > 0) {
                t->cursor_col--;
            }
        } else if (c == '\t') {
            int next = (t->cursor_col + 8) & ~7;
            if (next >= t->cols) next = t->cols - 1;
            t->cursor_col = next;
        } else if (c >= 0x20 && c <= 0x7e) {
            t->cells[t->cursor_row][t->cursor_col] = c;
            advance_cursor(t);
        }
        break;

    case TERM_PARSE_ESC:
        if (c == '[') {
            t->parse_state = TERM_PARSE_CSI;
            t->csi_len = 0;
        } else {
            t->parse_state = TERM_PARSE_NORMAL;
        }
        break;

    case TERM_PARSE_CSI:
        if ((c >= '0' && c <= '9') || c == ';') {
            if (t->csi_len < (int)sizeof(t->csi_buf) - 1) {
                t->csi_buf[t->csi_len++] = c;
            }
        } else {
            handle_csi(t, c);
            t->parse_state = TERM_PARSE_NORMAL;
        }
        break;
    }
}

void term_feed(term_state* t, const char* data, int len) {
    for (int i = 0; i < len; i++) {
        feed_byte(t, data[i]);
    }
    t->dirty = 1;
}
