#include "term.h"
#include <string.h>

static const term_attr_t ATTR_DEFAULT = { 0, 0 };

static void clear_row_attrs(term_state_t *t, int row, int from, int to) {
    for (int c = from; c < to; c++)
        t->attrs[row][c] = ATTR_DEFAULT;
}

static void scroll_up(term_state_t *t) {
    memmove(&t->cells[0], &t->cells[1],
            (size_t)(t->rows - 1) * TERM_MAX_COLS);
    memmove(&t->attrs[0], &t->attrs[1],
            (size_t)(t->rows - 1) * TERM_MAX_COLS * sizeof(term_attr_t));
    memset(&t->cells[t->rows - 1], ' ', (size_t)t->cols);
    clear_row_attrs(t, t->rows - 1, 0, t->cols);
}

static void cursor_down(term_state_t *t) {
    if (t->cursor_row < t->rows - 1) {
        t->cursor_row++;
    } else {
        scroll_up(t);
    }
}

static void put_char(term_state_t *t, char ch) {
    if (t->cursor_col >= t->cols) {
        t->cursor_col = 0;
        cursor_down(t);
    }
    t->cells[t->cursor_row][t->cursor_col] = ch;
    t->attrs[t->cursor_row][t->cursor_col].fg = t->cur_fg;
    t->attrs[t->cursor_row][t->cursor_col].bg = t->cur_bg;
    t->cursor_col++;
}

static int csi_param(term_state_t *t, int idx, int def) {
    if (idx >= t->csi_param_count) return def;
    return t->csi_params[idx] == 0 ? def : t->csi_params[idx];
}

static void sgr_dispatch(term_state_t *t) {
    if (t->csi_param_count == 1 && t->csi_params[0] == 0) {
        t->cur_fg = 0;
        t->cur_bg = 0;
        t->cur_bold = 0;
        return;
    }
    for (int i = 0; i < t->csi_param_count; i++) {
        int p = t->csi_params[i];
        if (p == 0) {
            t->cur_fg = 0;
            t->cur_bg = 0;
            t->cur_bold = 0;
        } else if (p == 1) {
            t->cur_bold = 1;
        } else if (p == 22) {
            t->cur_bold = 0;
        } else if (p >= 30 && p <= 37) {
            t->cur_fg = (uint8_t)(p - 30 + 1);
            if (t->cur_bold) t->cur_fg += 8;
        } else if (p == 39) {
            t->cur_fg = 0;
        } else if (p >= 40 && p <= 47) {
            t->cur_bg = (uint8_t)(p - 40 + 1);
        } else if (p == 49) {
            t->cur_bg = 0;
        } else if (p >= 90 && p <= 97) {
            t->cur_fg = (uint8_t)(p - 90 + 9);
        } else if (p >= 100 && p <= 107) {
            t->cur_bg = (uint8_t)(p - 100 + 9);
        }
    }
}

static void csi_dispatch(term_state_t *t, char cmd) {
    int p0;
    switch (cmd) {
    case 'K':
        p0 = csi_param(t, 0, 0);
        if (p0 == 2) {
            memset(&t->cells[t->cursor_row], ' ', (size_t)t->cols);
            clear_row_attrs(t, t->cursor_row, 0, t->cols);
        } else {
            for (int c = t->cursor_col; c < t->cols; c++) {
                t->cells[t->cursor_row][c] = ' ';
                t->attrs[t->cursor_row][c] = ATTR_DEFAULT;
            }
        }
        break;
    case 'C':
        p0 = csi_param(t, 0, 1);
        t->cursor_col += p0;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;
    case 'D':
        p0 = csi_param(t, 0, 1);
        t->cursor_col -= p0;
        if (t->cursor_col < 0) t->cursor_col = 0;
        break;
    case 'H':
        t->cursor_row = csi_param(t, 0, 1) - 1;
        t->cursor_col = csi_param(t, 1, 1) - 1;
        if (t->cursor_row < 0) t->cursor_row = 0;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        if (t->cursor_col < 0) t->cursor_col = 0;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;
    case 'J':
        p0 = csi_param(t, 0, 0);
        if (p0 == 2) {
            for (int r = 0; r < t->rows; r++) {
                memset(&t->cells[r], ' ', (size_t)t->cols);
                clear_row_attrs(t, r, 0, t->cols);
            }
            t->cursor_row = 0;
            t->cursor_col = 0;
        }
        break;
    case 'm':
        sgr_dispatch(t);
        break;
    default:
        break;
    }
}

static void csi_push_param(term_state_t *t) {
    if (t->csi_param_count < TERM_CSI_MAX_PARAMS) {
        t->csi_params[t->csi_param_count++] = t->csi_current_param;
    }
    t->csi_current_param = 0;
}

static void feed_byte(term_state_t *t, unsigned char ch) {
    switch (t->state) {
    case TERM_ST_NORMAL:
        if (ch >= 0x20 && ch <= 0x7E) {
            put_char(t, (char)ch);
        } else if (ch == '\r') {
            t->cursor_col = 0;
        } else if (ch == '\n') {
            cursor_down(t);
        } else if (ch == '\b') {
            if (t->cursor_col > 0) t->cursor_col--;
        } else if (ch == '\t') {
            int next = (t->cursor_col + 8) & ~7;
            if (next > t->cols) next = t->cols;
            t->cursor_col = next;
        } else if (ch == '\x1b') {
            t->state = TERM_ST_ESC;
        }
        break;

    case TERM_ST_ESC:
        if (ch == '[') {
            t->csi_param_count = 0;
            t->csi_current_param = 0;
            t->state = TERM_ST_CSI;
        } else {
            t->state = TERM_ST_NORMAL;
        }
        break;

    case TERM_ST_CSI:
        if (ch >= '0' && ch <= '9') {
            t->csi_current_param = t->csi_current_param * 10 + (ch - '0');
        } else if (ch == ';') {
            csi_push_param(t);
        } else if (ch >= 0x40 && ch <= 0x7E) {
            csi_push_param(t);
            csi_dispatch(t, (char)ch);
            t->state = TERM_ST_NORMAL;
        } else if (ch == '\x1b') {
            t->state = TERM_ST_ESC;
        }
        break;
    }
}

void term_init(term_state_t *t, int rows, int cols) {
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    t->rows = rows;
    t->cols = cols;
    t->cursor_row = 0;
    t->cursor_col = 0;
    t->dirty = 1;
    t->cur_fg = 0;
    t->cur_bg = 0;
    t->cur_bold = 0;
    t->state = TERM_ST_NORMAL;
    t->csi_param_count = 0;
    t->csi_current_param = 0;
    for (int r = 0; r < TERM_MAX_ROWS; r++) {
        memset(&t->cells[r], ' ', TERM_MAX_COLS);
        memset(&t->attrs[r], 0, TERM_MAX_COLS * sizeof(term_attr_t));
    }
}

void term_feed(term_state_t *t, const char *data, int len) {
    for (int i = 0; i < len; i++)
        feed_byte(t, (unsigned char)data[i]);
    t->dirty = 1;
}
