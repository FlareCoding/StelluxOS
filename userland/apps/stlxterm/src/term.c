#include "term.h"
#include <string.h>

/* Standard 8-color ANSI palette + bright variants (Catppuccin Mocha inspired) */
static const uint32_t ANSI_COLORS[16] = {
    0xFF45475A, /* 0  black   (Surface1)   */
    0xFFF38BA8, /* 1  red     (Red)        */
    0xFFA6E3A1, /* 2  green   (Green)      */
    0xFFF9E2AF, /* 3  yellow  (Yellow)     */
    0xFF89B4FA, /* 4  blue    (Blue)       */
    0xFFF5C2E7, /* 5  magenta (Pink)       */
    0xFF94E2D5, /* 6  cyan    (Teal)       */
    0xFFBAC2DE, /* 7  white   (Subtext1)   */
    0xFF585B70, /* 8  bright black  (Surface2)  */
    0xFFF38BA8, /* 9  bright red    (Red)       */
    0xFFA6E3A1, /* 10 bright green  (Green)     */
    0xFFF9E2AF, /* 11 bright yellow (Yellow)    */
    0xFF89B4FA, /* 12 bright blue   (Blue)      */
    0xFFF5C2E7, /* 13 bright magenta(Pink)      */
    0xFF94E2D5, /* 14 bright cyan   (Teal)      */
    0xFFCDD6F4, /* 15 bright white  (Text)      */
};

void term_init(term_state* t, int rows, int cols,
               uint32_t default_fg, uint32_t default_bg) {
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    t->rows = rows;
    t->cols = cols;
    t->cursor_row = 0;
    t->cursor_col = 0;
    t->dirty = 1;
    t->parse_state = TERM_PARSE_NORMAL;
    t->csi_len = 0;
    t->default_fg = default_fg;
    t->default_bg = default_bg;
    t->current_attr.fg = default_fg;
    t->current_attr.bg = default_bg;
    t->current_attr.bold = 0;

    for (int r = 0; r < TERM_MAX_ROWS; r++) {
        for (int c = 0; c < TERM_MAX_COLS; c++) {
            t->cells[r][c].ch = ' ';
            t->cells[r][c].fg = default_fg;
            t->cells[r][c].bg = default_bg;
        }
    }
}

static void clear_row(term_state* t, int row) {
    for (int c = 0; c < t->cols; c++) {
        t->cells[row][c].ch = ' ';
        t->cells[row][c].fg = t->default_fg;
        t->cells[row][c].bg = t->default_bg;
    }
}

static void scroll_up(term_state* t) {
    for (int r = 0; r < t->rows - 1; r++) {
        memcpy(t->cells[r], t->cells[r + 1], (size_t)t->cols * sizeof(term_cell));
    }
    clear_row(t, t->rows - 1);
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

static int parse_params(const char* buf, int len, int* params, int max_params) {
    int count = 0;
    int val = -1;
    for (int i = 0; i < len && count < max_params; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            if (val < 0) val = 0;
            val = val * 10 + (buf[i] - '0');
        } else if (buf[i] == ';') {
            params[count++] = (val < 0) ? 0 : val;
            val = -1;
        }
    }
    if (count < max_params) {
        params[count++] = (val < 0) ? 0 : val;
    }
    return count;
}

static void handle_sgr(term_state* t, int* params, int count) {
    for (int i = 0; i < count; i++) {
        int p = params[i];
        if (p == 0) {
            t->current_attr.fg = t->default_fg;
            t->current_attr.bg = t->default_bg;
            t->current_attr.bold = 0;
        } else if (p == 1) {
            t->current_attr.bold = 1;
        } else if (p == 22) {
            t->current_attr.bold = 0;
        } else if (p >= 30 && p <= 37) {
            int idx = p - 30;
            if (t->current_attr.bold) idx += 8;
            t->current_attr.fg = ANSI_COLORS[idx];
        } else if (p == 39) {
            t->current_attr.fg = t->default_fg;
        } else if (p >= 40 && p <= 47) {
            t->current_attr.bg = ANSI_COLORS[p - 40];
        } else if (p == 49) {
            t->current_attr.bg = t->default_bg;
        } else if (p >= 90 && p <= 97) {
            t->current_attr.fg = ANSI_COLORS[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            t->current_attr.bg = ANSI_COLORS[p - 100 + 8];
        }
    }
}

static void handle_csi(term_state* t, char final) {
    int params[TERM_CSI_MAX_PARAMS];
    int count = parse_params(t->csi_buf, t->csi_len, params, TERM_CSI_MAX_PARAMS);
    int p1 = (count > 0 && params[0] > 0) ? params[0] : 1;

    switch (final) {
    case 'A':
        t->cursor_row -= p1;
        if (t->cursor_row < 0) t->cursor_row = 0;
        break;
    case 'B':
        t->cursor_row += p1;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        break;
    case 'C':
        t->cursor_col += p1;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;
    case 'D':
        t->cursor_col -= p1;
        if (t->cursor_col < 0) t->cursor_col = 0;
        break;
    case 'H': {
        int row = (count > 0 && params[0] > 0) ? params[0] - 1 : 0;
        int col = (count > 1 && params[1] > 0) ? params[1] - 1 : 0;
        if (row >= t->rows) row = t->rows - 1;
        if (col >= t->cols) col = t->cols - 1;
        t->cursor_row = row;
        t->cursor_col = col;
        break;
    }
    case 'F':
        t->cursor_col = t->cols - 1;
        break;
    case 'J': {
        int mode = (count > 0) ? params[0] : 0;
        if (mode == 2) {
            for (int r = 0; r < t->rows; r++) clear_row(t, r);
            t->cursor_row = 0;
            t->cursor_col = 0;
        }
        break;
    }
    case 'K': {
        int mode = (count > 0) ? params[0] : 0;
        if (mode == 0) {
            for (int c = t->cursor_col; c < t->cols; c++) {
                t->cells[t->cursor_row][c].ch = ' ';
                t->cells[t->cursor_row][c].fg = t->default_fg;
                t->cells[t->cursor_row][c].bg = t->default_bg;
            }
        } else if (mode == 1) {
            for (int c = 0; c <= t->cursor_col; c++) {
                t->cells[t->cursor_row][c].ch = ' ';
                t->cells[t->cursor_row][c].fg = t->default_fg;
                t->cells[t->cursor_row][c].bg = t->default_bg;
            }
        } else if (mode == 2) {
            clear_row(t, t->cursor_row);
        }
        break;
    }
    case 'm':
        handle_sgr(t, params, count);
        break;
    case '~':
        break;
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
            t->cells[t->cursor_row][t->cursor_col].ch = c;
            t->cells[t->cursor_row][t->cursor_col].fg = t->current_attr.fg;
            t->cells[t->cursor_row][t->cursor_col].bg = t->current_attr.bg;
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
            if (t->csi_len < TERM_CSI_MAX - 1) {
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
