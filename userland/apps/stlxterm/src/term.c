#include "term.h"
#include <stdlib.h>
#include <string.h>

static const uint32_t ANSI_COLORS[16] = {
    0xFF45475A, 0xFFF38BA8, 0xFFA6E3A1, 0xFFF9E2AF,
    0xFF89B4FA, 0xFFF5C2E7, 0xFF94E2D5, 0xFFBAC2DE,
    0xFF585B70, 0xFFF38BA8, 0xFFA6E3A1, 0xFFF9E2AF,
    0xFF89B4FA, 0xFFF5C2E7, 0xFF94E2D5, 0xFFCDD6F4,
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
    t->scroll_offset = 0;

    memset(t->chars, ' ', sizeof(t->chars));
    for (int r = 0; r < TERM_MAX_ROWS; r++) {
        for (int c = 0; c < TERM_MAX_COLS; c++) {
            t->fg[r][c] = default_fg;
            t->bg[r][c] = default_bg;
        }
    }

    t->scrollback = (term_scrollback*)calloc(1, sizeof(term_scrollback));
    if (t->scrollback) {
        t->scrollback->head = 0;
        t->scrollback->count = 0;
    }
}

static void clear_row(term_state* t, int row) {
    memset(t->chars[row], ' ', (size_t)t->cols);
    for (int c = 0; c < t->cols; c++) {
        t->fg[row][c] = t->default_fg;
        t->bg[row][c] = t->default_bg;
    }
}

static void save_line_to_scrollback(term_state* t, int row) {
    if (!t->scrollback) return;

    term_scrollback* sb = t->scrollback;
    term_line* dst = &sb->lines[sb->head];

    memcpy(dst->ch, t->chars[row], (size_t)t->cols);
    memcpy(dst->fg, t->fg[row], (size_t)t->cols * sizeof(uint32_t));
    memcpy(dst->bg, t->bg[row], (size_t)t->cols * sizeof(uint32_t));

    for (int c = t->cols; c < TERM_MAX_COLS; c++) {
        dst->ch[c] = ' ';
        dst->fg[c] = t->default_fg;
        dst->bg[c] = t->default_bg;
    }

    sb->head = (sb->head + 1) % TERM_SCROLLBACK_LINES;
    if (sb->count < TERM_SCROLLBACK_LINES) {
        sb->count++;
    }
}

static void scroll_up(term_state* t) {
    save_line_to_scrollback(t, 0);

    for (int r = 0; r < t->rows - 1; r++) {
        memcpy(t->chars[r], t->chars[r + 1], (size_t)t->cols);
        memcpy(t->fg[r], t->fg[r + 1], (size_t)t->cols * sizeof(uint32_t));
        memcpy(t->bg[r], t->bg[r + 1], (size_t)t->cols * sizeof(uint32_t));
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
        int row = t->cursor_row;
        if (mode == 0) {
            for (int c = t->cursor_col; c < t->cols; c++) {
                t->chars[row][c] = ' ';
                t->fg[row][c] = t->default_fg;
                t->bg[row][c] = t->default_bg;
            }
        } else if (mode == 1) {
            for (int c = 0; c <= t->cursor_col; c++) {
                t->chars[row][c] = ' ';
                t->fg[row][c] = t->default_fg;
                t->bg[row][c] = t->default_bg;
            }
        } else if (mode == 2) {
            clear_row(t, row);
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
            if (t->cursor_col > 0) t->cursor_col--;
        } else if (c == '\t') {
            int next = (t->cursor_col + 8) & ~7;
            if (next >= t->cols) next = t->cols - 1;
            t->cursor_col = next;
        } else if (c >= 0x20 && c <= 0x7e) {
            int row = t->cursor_row;
            int col = t->cursor_col;
            t->chars[row][col] = c;
            t->fg[row][col] = t->current_attr.fg;
            t->bg[row][col] = t->current_attr.bg;
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
            if (t->csi_len < TERM_CSI_MAX - 1)
                t->csi_buf[t->csi_len++] = c;
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
    if (t->scroll_offset > 0) {
        term_scroll_to_bottom(t);
    }
    t->dirty = 1;
}

void term_scroll_up_view(term_state* t, int lines) {
    if (!t->scrollback) return;
    t->scroll_offset += lines;
    if (t->scroll_offset > t->scrollback->count) {
        t->scroll_offset = t->scrollback->count;
    }
    t->dirty = 1;
}

void term_scroll_down_view(term_state* t, int lines) {
    t->scroll_offset -= lines;
    if (t->scroll_offset < 0) {
        t->scroll_offset = 0;
    }
    t->dirty = 1;
}

void term_scroll_to_bottom(term_state* t) {
    t->scroll_offset = 0;
    t->dirty = 1;
}

int term_get_display_line(term_state* t, int screen_row,
                          const char** out_ch,
                          const uint32_t** out_fg,
                          const uint32_t** out_bg) {
    if (t->scroll_offset == 0) {
        *out_ch = t->chars[screen_row];
        *out_fg = t->fg[screen_row];
        *out_bg = t->bg[screen_row];
        return 1;
    }

    int scrollback_rows_visible = t->scroll_offset;
    if (scrollback_rows_visible > t->rows) {
        scrollback_rows_visible = t->rows;
    }

    if (screen_row < scrollback_rows_visible) {
        if (!t->scrollback) {
            *out_ch = t->chars[screen_row];
            *out_fg = t->fg[screen_row];
            *out_bg = t->bg[screen_row];
            return 1;
        }
        int sb_index = t->scroll_offset - scrollback_rows_visible + screen_row;
        int pos = t->scrollback->head - t->scrollback->count + (t->scrollback->count - 1 - sb_index);
        if (pos < 0) pos += TERM_SCROLLBACK_LINES;
        pos = pos % TERM_SCROLLBACK_LINES;

        *out_ch = t->scrollback->lines[pos].ch;
        *out_fg = t->scrollback->lines[pos].fg;
        *out_bg = t->scrollback->lines[pos].bg;
        return 0;
    }

    int live_row = screen_row - scrollback_rows_visible;
    if (live_row < 0) live_row = 0;
    if (live_row >= t->rows) live_row = t->rows - 1;

    *out_ch = t->chars[live_row];
    *out_fg = t->fg[live_row];
    *out_bg = t->bg[live_row];
    return 1;
}
