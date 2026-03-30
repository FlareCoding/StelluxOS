#include "term.h"
#include <string.h>

static const term_attr_t ATTR_DEFAULT = { 0, 0 };

static void clear_row_range(term_state_t *t, int row, int from, int to) {
    for (int c = from; c < to; c++) {
        t->cells[row][c] = ' ';
        t->attrs[row][c] = ATTR_DEFAULT;
    }
}

static void clear_row(term_state_t *t, int row) {
    clear_row_range(t, row, 0, t->cols);
}

/* Scroll up within the scroll region by n lines */
static void scroll_up_region(term_state_t *t, int n) {
    int top = t->scroll_top;
    int bot = t->scroll_bottom;
    if (n <= 0) return;
    int region_height = bot - top + 1;
    if (n > region_height) n = region_height;

    /* Move lines up */
    for (int r = top; r <= bot - n; r++) {
        memcpy(&t->cells[r], &t->cells[r + n], (size_t)t->cols);
        memcpy(&t->attrs[r], &t->attrs[r + n], (size_t)t->cols * sizeof(term_attr_t));
    }
    /* Clear newly exposed lines at bottom */
    for (int r = bot - n + 1; r <= bot; r++) {
        clear_row(t, r);
    }
}

/* Scroll down within the scroll region by n lines */
static void scroll_down_region(term_state_t *t, int n) {
    int top = t->scroll_top;
    int bot = t->scroll_bottom;
    if (n <= 0) return;
    int region_height = bot - top + 1;
    if (n > region_height) n = region_height;

    /* Move lines down */
    for (int r = bot; r >= top + n; r--) {
        memcpy(&t->cells[r], &t->cells[r - n], (size_t)t->cols);
        memcpy(&t->attrs[r], &t->attrs[r - n], (size_t)t->cols * sizeof(term_attr_t));
    }
    /* Clear newly exposed lines at top */
    for (int r = top; r < top + n; r++) {
        clear_row(t, r);
    }
}

static void cursor_down(term_state_t *t) {
    if (t->cursor_row < t->scroll_bottom) {
        t->cursor_row++;
    } else if (t->cursor_row == t->scroll_bottom) {
        scroll_up_region(t, 1);
    }
    /* If cursor is outside scroll region, just increment if possible */
    else if (t->cursor_row < t->rows - 1) {
        t->cursor_row++;
    }
}

static void cursor_up_scroll(term_state_t *t) {
    if (t->cursor_row > t->scroll_top) {
        t->cursor_row--;
    } else if (t->cursor_row == t->scroll_top) {
        scroll_down_region(t, 1);
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
    if (t->csi_param_count == 0 ||
        (t->csi_param_count == 1 && t->csi_params[0] == 0)) {
        t->cur_fg = 0;
        t->cur_bg = 0;
        t->cur_bold = 0;
        t->cur_reverse = 0;
        return;
    }
    for (int i = 0; i < t->csi_param_count; i++) {
        int p = t->csi_params[i];
        if (p == 0) {
            t->cur_fg = 0;
            t->cur_bg = 0;
            t->cur_bold = 0;
            t->cur_reverse = 0;
        } else if (p == 1) {
            t->cur_bold = 1;
        } else if (p == 7) {
            t->cur_reverse = 1;
        } else if (p == 22) {
            t->cur_bold = 0;
        } else if (p == 27) {
            t->cur_reverse = 0;
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

/* Switch to alternate screen buffer */
static void enter_alt_screen(term_state_t *t) {
    if (t->using_alt_screen) return;
    /* Save main screen */
    memcpy(t->alt_cells, t->cells, sizeof(t->cells));
    memcpy(t->alt_attrs, t->attrs, sizeof(t->attrs));
    t->alt_cursor_row = t->cursor_row;
    t->alt_cursor_col = t->cursor_col;
    t->using_alt_screen = 1;
    /* Clear alt screen */
    for (int r = 0; r < t->rows; r++)
        clear_row(t, r);
    t->cursor_row = 0;
    t->cursor_col = 0;
}

/* Switch back to main screen buffer */
static void leave_alt_screen(term_state_t *t) {
    if (!t->using_alt_screen) return;
    /* Restore main screen */
    memcpy(t->cells, t->alt_cells, sizeof(t->cells));
    memcpy(t->attrs, t->alt_attrs, sizeof(t->attrs));
    t->cursor_row = t->alt_cursor_row;
    t->cursor_col = t->alt_cursor_col;
    t->using_alt_screen = 0;
}

static void csi_dispatch_private(term_state_t *t, char cmd) {
    int p0 = csi_param(t, 0, 0);

    if (cmd == 'h') {
        /* DEC Private Mode Set */
        switch (p0) {
        case 25:  /* DECTCEM: Show cursor */
            t->cursor_visible = 1;
            break;
        case 1049: /* Alternate screen buffer + save cursor */
            t->saved_cursor_row = t->cursor_row;
            t->saved_cursor_col = t->cursor_col;
            t->saved_fg = t->cur_fg;
            t->saved_bg = t->cur_bg;
            t->saved_bold = t->cur_bold;
            enter_alt_screen(t);
            break;
        case 47:  /* Alternate screen buffer (no save) */
        case 1047:
            enter_alt_screen(t);
            break;
        }
    } else if (cmd == 'l') {
        /* DEC Private Mode Reset */
        switch (p0) {
        case 25:  /* DECTCEM: Hide cursor */
            t->cursor_visible = 0;
            break;
        case 1049: /* Restore main screen + restore cursor */
            leave_alt_screen(t);
            t->cursor_row = t->saved_cursor_row;
            t->cursor_col = t->saved_cursor_col;
            t->cur_fg = t->saved_fg;
            t->cur_bg = t->saved_bg;
            t->cur_bold = t->saved_bold;
            break;
        case 47:  /* Restore main screen (no restore cursor) */
        case 1047:
            leave_alt_screen(t);
            break;
        }
    }
}

static void csi_dispatch(term_state_t *t, char cmd) {
    int p0, p1, n;
    switch (cmd) {
    case 'A': /* Cursor Up */
        p0 = csi_param(t, 0, 1);
        t->cursor_row -= p0;
        if (t->cursor_row < 0) t->cursor_row = 0;
        break;

    case 'B': /* Cursor Down */
        p0 = csi_param(t, 0, 1);
        t->cursor_row += p0;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        break;

    case 'C': /* Cursor Forward */
        p0 = csi_param(t, 0, 1);
        t->cursor_col += p0;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;

    case 'D': /* Cursor Back */
        p0 = csi_param(t, 0, 1);
        t->cursor_col -= p0;
        if (t->cursor_col < 0) t->cursor_col = 0;
        break;

    case 'E': /* Cursor Next Line */
        p0 = csi_param(t, 0, 1);
        t->cursor_row += p0;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        t->cursor_col = 0;
        break;

    case 'F': /* Cursor Previous Line */
        p0 = csi_param(t, 0, 1);
        t->cursor_row -= p0;
        if (t->cursor_row < 0) t->cursor_row = 0;
        t->cursor_col = 0;
        break;

    case 'G': /* Cursor Horizontal Absolute */
        p0 = csi_param(t, 0, 1);
        t->cursor_col = p0 - 1;
        if (t->cursor_col < 0) t->cursor_col = 0;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;

    case 'H': /* Cursor Position */
    case 'f': /* Horizontal and Vertical Position */
        t->cursor_row = csi_param(t, 0, 1) - 1;
        t->cursor_col = csi_param(t, 1, 1) - 1;
        if (t->cursor_row < 0) t->cursor_row = 0;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        if (t->cursor_col < 0) t->cursor_col = 0;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;

    case 'J': /* Erase in Display */
        p0 = csi_param(t, 0, 0);
        if (p0 == 0) {
            /* Clear from cursor to end of screen */
            clear_row_range(t, t->cursor_row, t->cursor_col, t->cols);
            for (int r = t->cursor_row + 1; r < t->rows; r++)
                clear_row(t, r);
        } else if (p0 == 1) {
            /* Clear from start of screen to cursor */
            for (int r = 0; r < t->cursor_row; r++)
                clear_row(t, r);
            clear_row_range(t, t->cursor_row, 0, t->cursor_col + 1);
        } else if (p0 == 2) {
            /* Clear entire screen */
            for (int r = 0; r < t->rows; r++)
                clear_row(t, r);
            t->cursor_row = 0;
            t->cursor_col = 0;
        } else if (p0 == 3) {
            /* Clear entire screen + scrollback (same as 2 for us) */
            for (int r = 0; r < t->rows; r++)
                clear_row(t, r);
            t->cursor_row = 0;
            t->cursor_col = 0;
        }
        break;

    case 'K': /* Erase in Line */
        p0 = csi_param(t, 0, 0);
        if (p0 == 0) {
            /* Clear from cursor to end of line */
            clear_row_range(t, t->cursor_row, t->cursor_col, t->cols);
        } else if (p0 == 1) {
            /* Clear from start of line to cursor */
            clear_row_range(t, t->cursor_row, 0, t->cursor_col + 1);
        } else if (p0 == 2) {
            /* Clear entire line */
            clear_row(t, t->cursor_row);
        }
        break;

    case 'L': /* Insert Lines */
        p0 = csi_param(t, 0, 1);
        if (t->cursor_row >= t->scroll_top && t->cursor_row <= t->scroll_bottom) {
            /* Insert p0 lines at cursor, scrolling down within region */
            n = t->scroll_bottom - t->cursor_row + 1;
            if (p0 > n) p0 = n;
            for (int r = t->scroll_bottom; r >= t->cursor_row + p0; r--) {
                memcpy(&t->cells[r], &t->cells[r - p0], (size_t)t->cols);
                memcpy(&t->attrs[r], &t->attrs[r - p0], (size_t)t->cols * sizeof(term_attr_t));
            }
            for (int r = t->cursor_row; r < t->cursor_row + p0 && r <= t->scroll_bottom; r++) {
                clear_row(t, r);
            }
        }
        break;

    case 'M': /* Delete Lines */
        p0 = csi_param(t, 0, 1);
        if (t->cursor_row >= t->scroll_top && t->cursor_row <= t->scroll_bottom) {
            n = t->scroll_bottom - t->cursor_row + 1;
            if (p0 > n) p0 = n;
            for (int r = t->cursor_row; r <= t->scroll_bottom - p0; r++) {
                memcpy(&t->cells[r], &t->cells[r + p0], (size_t)t->cols);
                memcpy(&t->attrs[r], &t->attrs[r + p0], (size_t)t->cols * sizeof(term_attr_t));
            }
            for (int r = t->scroll_bottom - p0 + 1; r <= t->scroll_bottom; r++) {
                clear_row(t, r);
            }
        }
        break;

    case 'P': /* Delete Characters */
        p0 = csi_param(t, 0, 1);
        n = t->cols - t->cursor_col;
        if (p0 > n) p0 = n;
        if (p0 > 0) {
            memmove(&t->cells[t->cursor_row][t->cursor_col],
                    &t->cells[t->cursor_row][t->cursor_col + p0],
                    (size_t)(t->cols - t->cursor_col - p0));
            memmove(&t->attrs[t->cursor_row][t->cursor_col],
                    &t->attrs[t->cursor_row][t->cursor_col + p0],
                    (size_t)(t->cols - t->cursor_col - p0) * sizeof(term_attr_t));
            for (int c = t->cols - p0; c < t->cols; c++) {
                t->cells[t->cursor_row][c] = ' ';
                t->attrs[t->cursor_row][c] = ATTR_DEFAULT;
            }
        }
        break;

    case '@': /* Insert Characters */
        p0 = csi_param(t, 0, 1);
        n = t->cols - t->cursor_col;
        if (p0 > n) p0 = n;
        if (p0 > 0) {
            memmove(&t->cells[t->cursor_row][t->cursor_col + p0],
                    &t->cells[t->cursor_row][t->cursor_col],
                    (size_t)(t->cols - t->cursor_col - p0));
            memmove(&t->attrs[t->cursor_row][t->cursor_col + p0],
                    &t->attrs[t->cursor_row][t->cursor_col],
                    (size_t)(t->cols - t->cursor_col - p0) * sizeof(term_attr_t));
            for (int c = t->cursor_col; c < t->cursor_col + p0; c++) {
                t->cells[t->cursor_row][c] = ' ';
                t->attrs[t->cursor_row][c] = ATTR_DEFAULT;
            }
        }
        break;

    case 'X': /* Erase Characters */
        p0 = csi_param(t, 0, 1);
        n = t->cols - t->cursor_col;
        if (p0 > n) p0 = n;
        for (int c = t->cursor_col; c < t->cursor_col + p0; c++) {
            t->cells[t->cursor_row][c] = ' ';
            t->attrs[t->cursor_row][c] = ATTR_DEFAULT;
        }
        break;

    case 'S': /* Scroll Up */
        p0 = csi_param(t, 0, 1);
        scroll_up_region(t, p0);
        break;

    case 'T': /* Scroll Down */
        p0 = csi_param(t, 0, 1);
        scroll_down_region(t, p0);
        break;

    case 'd': /* Vertical Line Position Absolute */
        p0 = csi_param(t, 0, 1);
        t->cursor_row = p0 - 1;
        if (t->cursor_row < 0) t->cursor_row = 0;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        break;

    case 'r': /* DECSTBM: Set Scrolling Region */
        p0 = csi_param(t, 0, 1);
        p1 = csi_param(t, 1, t->rows);
        t->scroll_top = p0 - 1;
        t->scroll_bottom = p1 - 1;
        if (t->scroll_top < 0) t->scroll_top = 0;
        if (t->scroll_bottom >= t->rows) t->scroll_bottom = t->rows - 1;
        if (t->scroll_top >= t->scroll_bottom) {
            t->scroll_top = 0;
            t->scroll_bottom = t->rows - 1;
        }
        /* Move cursor to home */
        t->cursor_row = 0;
        t->cursor_col = 0;
        break;

    case 's': /* Save Cursor Position */
        t->saved_cursor_row = t->cursor_row;
        t->saved_cursor_col = t->cursor_col;
        t->saved_fg = t->cur_fg;
        t->saved_bg = t->cur_bg;
        t->saved_bold = t->cur_bold;
        break;

    case 'u': /* Restore Cursor Position */
        t->cursor_row = t->saved_cursor_row;
        t->cursor_col = t->saved_cursor_col;
        t->cur_fg = t->saved_fg;
        t->cur_bg = t->saved_bg;
        t->cur_bold = t->saved_bold;
        if (t->cursor_row < 0) t->cursor_row = 0;
        if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
        if (t->cursor_col < 0) t->cursor_col = 0;
        if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        break;

    case 'm': /* SGR */
        sgr_dispatch(t);
        break;

    case 'n': /* Device Status Report */
        /* DSR 6 = cursor position report - we can't respond without write access
           to the pty master, so just ignore */
        break;

    case 'c': /* Device Attributes - ignore */
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
        } else if (ch == '\x0e' || ch == '\x0f') {
            /* SI/SO - shift in/out, ignore */
        } else if (ch == '\x07') {
            /* BEL - bell, ignore */
        }
        break;

    case TERM_ST_ESC:
        if (ch == '[') {
            t->csi_param_count = 0;
            t->csi_current_param = 0;
            t->csi_private = 0;
            t->state = TERM_ST_CSI;
        } else if (ch == '7') {
            /* DECSC: Save Cursor */
            t->saved_cursor_row = t->cursor_row;
            t->saved_cursor_col = t->cursor_col;
            t->saved_fg = t->cur_fg;
            t->saved_bg = t->cur_bg;
            t->saved_bold = t->cur_bold;
            t->state = TERM_ST_NORMAL;
        } else if (ch == '8') {
            /* DECRC: Restore Cursor */
            t->cursor_row = t->saved_cursor_row;
            t->cursor_col = t->saved_cursor_col;
            t->cur_fg = t->saved_fg;
            t->cur_bg = t->saved_bg;
            t->cur_bold = t->saved_bold;
            if (t->cursor_row < 0) t->cursor_row = 0;
            if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
            if (t->cursor_col < 0) t->cursor_col = 0;
            if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
            t->state = TERM_ST_NORMAL;
        } else if (ch == 'M') {
            /* RI: Reverse Index (cursor up, scroll down if at top) */
            cursor_up_scroll(t);
            t->state = TERM_ST_NORMAL;
        } else if (ch == 'D') {
            /* IND: Index (cursor down, scroll up if at bottom) */
            cursor_down(t);
            t->state = TERM_ST_NORMAL;
        } else if (ch == 'E') {
            /* NEL: Next Line */
            t->cursor_col = 0;
            cursor_down(t);
            t->state = TERM_ST_NORMAL;
        } else if (ch == 'c') {
            /* RIS: Full Reset */
            term_init(t, t->rows, t->cols);
        } else {
            /* Unknown ESC sequence, return to normal */
            t->state = TERM_ST_NORMAL;
        }
        break;

    case TERM_ST_CSI:
        if (ch == '?' && t->csi_param_count == 0 && t->csi_current_param == 0) {
            t->csi_private = 1;
        } else if (ch >= '0' && ch <= '9') {
            t->csi_current_param = t->csi_current_param * 10 + (ch - '0');
        } else if (ch == ';') {
            csi_push_param(t);
        } else if (ch >= 0x40 && ch <= 0x7E) {
            csi_push_param(t);
            if (t->csi_private) {
                csi_dispatch_private(t, (char)ch);
            } else {
                csi_dispatch(t, (char)ch);
            }
            t->state = TERM_ST_NORMAL;
        } else if (ch == '\x1b') {
            /* Interrupted CSI, start new escape */
            t->state = TERM_ST_ESC;
        } else {
            /* Unexpected char in CSI, bail */
            t->state = TERM_ST_NORMAL;
        }
        break;

    default:
        t->state = TERM_ST_NORMAL;
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
    t->cursor_visible = 1;
    t->dirty = 1;
    t->cur_fg = 0;
    t->cur_bg = 0;
    t->cur_bold = 0;
    t->cur_reverse = 0;
    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;
    t->saved_cursor_row = 0;
    t->saved_cursor_col = 0;
    t->saved_fg = 0;
    t->saved_bg = 0;
    t->saved_bold = 0;
    t->using_alt_screen = 0;
    t->state = TERM_ST_NORMAL;
    t->csi_param_count = 0;
    t->csi_current_param = 0;
    t->csi_private = 0;
    for (int r = 0; r < TERM_MAX_ROWS; r++) {
        memset(&t->cells[r], ' ', TERM_MAX_COLS);
        memset(&t->attrs[r], 0, TERM_MAX_COLS * sizeof(term_attr_t));
    }
    memset(t->alt_cells, ' ', sizeof(t->alt_cells));
    memset(t->alt_attrs, 0, sizeof(t->alt_attrs));
}

void term_feed(term_state_t *t, const char *data, int len) {
    for (int i = 0; i < len; i++)
        feed_byte(t, (unsigned char)data[i]);
    t->dirty = 1;
}
