/* stlxterm: the terminal emulator as a window protocol client. One
 * poll loop serves the window socket and the pty, damage is computed
 * by diffing the cell grid against the last painted frame, and the
 * process idles at zero wakeups when unfocused.
 */
#define _POSIX_C_SOURCE 199309L
#include <stlxwin/stlxwin.h>
#include <stlxwin/proto.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>
#include <stlxtheme/theme.h>
#include <stlx/proc.h>
#include <stlx/pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>

#include "keymap.h"
#include "paste.h"
#include "select.h"
#include "term.h"

#define STLX_TCSETS_RAW   0x7301

#define INITIAL_WIDTH     800
#define INITIAL_HEIGHT    600
#define MIN_COLS          20
#define MIN_ROWS          5
#define PADDING_X         14
#define PADDING_Y         10
#define BLINK_MS          500
#define PTY_DRAIN_MAX     16
#define SHELL_PATH        "/bin/shell"

/* Presses this close in time and place count as one double or triple click, Qt's defaults */
#define MULTI_CLICK_MS    400
#define MULTI_CLICK_SLOP  5

/* The echo of one write must fit the pty's output buffer, which the loop drains between writes */
#define INPUT_WRITE_MAX   2048

#define HID_USAGE_A       0x04
#define HID_USAGE_C       0x06
#define HID_USAGE_V       0x19
#define HID_USAGE_Z       0x1D
#define HID_USAGE_INSERT  0x49
#define CTRL_C_BYTE       0x03

static const uint32_t FG_PALETTE[17] = {
    STLXTHEME_TERMINAL_TEXT, // 0: default foreground
    0xFF3A342C, // 1: black (30)
    0xFFE0705F, // 2: red (31)
    0xFFA9C27A, // 3: green (32)
    0xFFE9B44C, // 4: yellow (33)
    0xFF8FA7C9, // 5: blue (34)
    0xFFC795B8, // 6: magenta (35)
    0xFF89BFB0, // 7: cyan (36)
    0xFFC4BAAB, // 8: white (37)
    0xFF6E675D, // 9: bright black (90)
    0xFFF08E7E, // 10: bright red (91)
    0xFFC2D795, // 11: bright green (92)
    0xFFFFC864, // 12: bright yellow (93)
    0xFFAFC3E0, // 13: bright blue (94)
    0xFFDDB1D0, // 14: bright magenta (95)
    0xFFA8D6C9, // 15: bright cyan (96)
    0xFFF6EEE1, // 16: bright white (97)
};

static const uint32_t BG_PALETTE[17] = {
    STLXTHEME_SHADE, // 0: default background
    0xFF3A342C, // 1: black (40)
    0xFFE0705F, // 2: red (41)
    0xFFA9C27A, // 3: green (42)
    0xFFE9B44C, // 4: yellow (43)
    0xFF8FA7C9, // 5: blue (44)
    0xFFC795B8, // 6: magenta (45)
    0xFF89BFB0, // 7: cyan (46)
    0xFFC4BAAB, // 8: white (47)
    0xFF6E675D, // 9: bright black (100)
    0xFFF08E7E, // 10: bright red (101)
    0xFFC2D795, // 11: bright green (102)
    0xFFFFC864, // 12: bright yellow (103)
    0xFFAFC3E0, // 13: bright blue (104)
    0xFFDDB1D0, // 14: bright magenta (105)
    0xFFA8D6C9, // 15: bright cyan (106)
    0xFFF6EEE1, // 16: bright white (107)
};

static term_state_t g_term;
static stlxgfx_font* g_font = NULL;
static int32_t g_ascent = 0;
static uint32_t g_cell_w = 8;
static uint32_t g_cell_h = 16;

/* The selection, the press dragging it and the clicks it counts. Button
 * events carry no modifiers, so a click reads the last key event's. */
static select_t g_select;
static int g_focused = 1;
static uint8_t g_modifiers = 0;
static int g_selecting = 0;
static int g_clicks = 0;
static uint64_t g_last_press_ms = 0;
static int32_t g_last_press_x = 0;
static int32_t g_last_press_y = 0;

/* Input the pty has not taken yet, such as the rest of a large paste.
 * Typed keys queue behind it so the program reads input in order. */
static char* g_input = NULL;
static size_t g_input_start = 0;
static size_t g_input_end = 0;
static size_t g_input_cap = 0;

/* Counted from the first byte of input: what the pty has taken, and the
 * newest paste's text between its bracketed paste markers */
static uint64_t g_input_taken = 0;
static uint64_t g_paste_text_from = 0;
static uint64_t g_paste_text_to = 0;

/* The grid as it was last painted, diffed against the live grid to
 * find damage. Each buffer of the pair also keeps the spans painted
 * into the other buffer since it was last painted itself, so it
 * catches up on exactly the frames it missed and nothing more. */
static char g_prev_cells[TERM_MAX_ROWS][TERM_MAX_COLS];
static term_attr_t g_prev_attrs[TERM_MAX_ROWS][TERM_MAX_COLS];
static int g_prev_valid = 0;
static int16_t g_span_min[TERM_MAX_ROWS];
static int16_t g_span_max[TERM_MAX_ROWS];
static int16_t g_stale_min[2][TERM_MAX_ROWS];
static int16_t g_stale_max[2][TERM_MAX_ROWS];
static int16_t g_prev_sel_from[TERM_MAX_ROWS];
static int16_t g_prev_sel_to[TERM_MAX_ROWS];
static int g_prev_sel_focused = 1;
static int g_last_cur_row = -1;
static int g_last_cur_col = -1;
static uint32_t g_last_buf_w = 0;
static uint32_t g_last_buf_h = 0;

/* Buffers the renderer has fully painted at least once. A slot seen
 * for the first time holds nothing at all, not a one frame stale
 * image, so span parity cannot ground it. */
static void* g_grounded[2] = { NULL, NULL };

static int grounded_index(void* pixels) {
    for (int i = 0; i < 2; i++) {
        if (g_grounded[i] == pixels) {
            return i;
        }
    }

    return -1;
}

static void ground_buffer(void* pixels) {
    if (grounded_index(pixels) >= 0) {
        return;
    }

    g_grounded[g_grounded[0] ? 1 : 0] = pixels;
}

static void stale_clear(int slot) {
    for (int r = 0; r < TERM_MAX_ROWS; r++) {
        g_stale_min[slot][r] = TERM_MAX_COLS;
        g_stale_max[slot][r] = -1;
    }
}

/* Adds this frame's spans to what the other buffer still has to repaint */
static void stale_add(int slot, const int16_t* min, const int16_t* max) {
    for (int r = 0; r < TERM_MAX_ROWS; r++) {
        if (max[r] < 0) {
            continue;
        }
        if (g_stale_min[slot][r] > min[r]) g_stale_min[slot][r] = min[r];
        if (g_stale_max[slot][r] < max[r]) g_stale_max[slot][r] = max[r];
    }
}

/* The cells of a row the selection lights as [*from, *to), both 0 when none */
static void selected_cells(int row, int* from, int* to) {
    if (!select_row_span(&g_select, &g_term, row, from, to)) {
        *from = 0;
        *to = 0;
    }
}

static void span_mark(int row, int c0, int c1) {
    if (row < 0 || row >= g_term.rows) {
        return;
    }
    if (g_span_min[row] > c0) g_span_min[row] = (int16_t)c0;
    if (g_span_max[row] < c1) g_span_max[row] = (int16_t)c1;
}

/* Paint one run of cells, background first, then the selection's tint,
 * glyphs on top, and the cursor cell inverted inside the run */
static void render_span(stlxgfx_surface_t* s, int row, int c0, int c1,
                        int cursor_on) {
    char ch_buf[2] = { 0, 0 };
    int sel_from, sel_to;
    selected_cells(row, &sel_from, &sel_to);

    for (int c = c0; c <= c1; c++) {
        int32_t px = PADDING_X + (int32_t)((uint32_t)c * g_cell_w);
        int32_t py = PADDING_Y + (int32_t)((uint32_t)row * g_cell_h);

        int cc = g_term.cursor_col < g_term.cols
               ? g_term.cursor_col : g_term.cols - 1;
        int at_cursor = cursor_on &&
                        row == g_term.cursor_row && c == cc;

        uint32_t bg = at_cursor ? STLXTHEME_TEXT
                    : BG_PALETTE[g_term.attrs[row][c].bg];
        stlxgfx_fill_rect(s, px, py, g_cell_w, g_cell_h, bg);
        if (!at_cursor && c >= sel_from && c < sel_to) {
            uint32_t tint = g_focused ? STLXTHEME_TERMINAL_SELECTION : STLXTHEME_TERMINAL_SELECTION_DIM;
            stlxgfx_fill_rect_blend(s, px, py, g_cell_w, g_cell_h, tint);
        }

        if (g_term.cells[row][c] != ' ') {
            uint32_t fg = at_cursor ? STLXTHEME_WINDOW_BG
                        : FG_PALETTE[g_term.attrs[row][c].fg];
            ch_buf[0] = g_term.cells[row][c];
            stlxgfx_draw_text(s, g_font, px, py + g_ascent, ch_buf, 1, fg);
        }
    }
}

/* Diff the grid against the last painted frame, repaint exactly the
 * changed cell runs, and commit their pixel rects. Returns 0, or -1
 * when no buffer can be acquired. */
static int render_frame(stlxwin_window* win, int cursor_on) {
    stlxwin_buffer* buf = stlxwin_begin_frame(win);
    if (!buf) {
        return -1;
    }

    for (int r = 0; r < TERM_MAX_ROWS; r++) {
        g_span_min[r] = TERM_MAX_COLS;
        g_span_max[r] = -1;
    }

    int resized = buf->width != g_last_buf_w || buf->height != g_last_buf_h;
    int full = !g_prev_valid || resized || grounded_index(buf->pixels) < 0;

    /* A size change replaces the slots, nothing stays grounded */
    if (resized) {
        g_grounded[0] = NULL;
        g_grounded[1] = NULL;
        stale_clear(0);
        stale_clear(1);
    }

    /* This frame's own changes, which the other buffer will need too */
    int16_t diff_min[TERM_MAX_ROWS];
    int16_t diff_max[TERM_MAX_ROWS];

    if (!full) {
        for (int r = 0; r < g_term.rows; r++) {
            if (memcmp(g_prev_cells[r], g_term.cells[r],
                       (size_t)g_term.cols) == 0 &&
                memcmp(g_prev_attrs[r], g_term.attrs[r],
                       (size_t)g_term.cols * sizeof(term_attr_t)) == 0) {
                continue;
            }

            for (int c = 0; c < g_term.cols; c++) {
                if (g_prev_cells[r][c] != g_term.cells[r][c] ||
                    memcmp(&g_prev_attrs[r][c], &g_term.attrs[r][c],
                           sizeof(term_attr_t)) != 0) {
                    span_mark(r, c, c);
                }
            }
        }

        /* Cursor cells are renderer state, not grid state: the spot it
         * left and the spot it occupies both repaint */
        if (g_last_cur_row >= 0) {
            span_mark(g_last_cur_row, g_last_cur_col, g_last_cur_col);
        }
        int cc = g_term.cursor_col < g_term.cols
               ? g_term.cursor_col : g_term.cols - 1;
        span_mark(g_term.cursor_row, cc, cc);

        /* So is the selection: cells it took or left repaint, and all of
         * it when focus changes its tint */
        for (int r = 0; r < g_term.rows; r++) {
            int from, to;
            selected_cells(r, &from, &to);
            int is_retinted = g_focused != g_prev_sel_focused && from < to;
            if (from == g_prev_sel_from[r] && to == g_prev_sel_to[r] && !is_retinted) {
                continue;
            }

            int lo = TERM_MAX_COLS;
            int hi = 0;
            if (from < to) {
                lo = from;
                hi = to;
            }

            if (g_prev_sel_from[r] < g_prev_sel_to[r]) {
                lo = g_prev_sel_from[r] < lo ? g_prev_sel_from[r] : lo;
                hi = g_prev_sel_to[r] > hi ? g_prev_sel_to[r] : hi;
            }

            span_mark(r, lo, hi - 1);
        }

        memcpy(diff_min, g_span_min, sizeof(diff_min));
        memcpy(diff_max, g_span_max, sizeof(diff_max));

        /* Whatever went into the other buffer since this one was last
         * painted repaints here as well, then this buffer is current */
        int slot = grounded_index(buf->pixels);
        for (int r = 0; r < g_term.rows; r++) {
            if (g_stale_max[slot][r] >= 0) {
                span_mark(r, g_stale_min[slot][r], g_stale_max[slot][r]);
            }
        }
        stale_clear(slot);
    }

    stlxgfx_surface_t* s = stlxgfx_surface_from_buffer(
        (uint8_t*)buf->pixels, buf->width, buf->height, buf->stride,
        32, 16, 8, 0);
    if (!s) {
        return -1;
    }

    s->flags |= STLXGFX_SURFACE_PREMULTIPLIED;

    int drawn_cursor = cursor_on && g_term.cursor_visible;

    stlxwin_rect rects[SWP_COMMIT_MAX_RECTS];
    uint32_t n_rects = 0;
    int overflow = 0;

    if (full) {
        stlxgfx_clear(s, STLXTHEME_SHADE);
        for (int r = 0; r < g_term.rows; r++) {
            render_span(s, r, 0, g_term.cols - 1, drawn_cursor);
        }
    } else {
        /* Runs on consecutive rows with identical extents merge into
         * one taller rect, and overflow falls back to a full commit */
        for (int r = 0; r < g_term.rows; r++) {
            if (g_span_max[r] < 0) {
                continue;
            }

            render_span(s, r, g_span_min[r], g_span_max[r], drawn_cursor);

            if (n_rects > 0) {
                stlxwin_rect* prev = &rects[n_rects - 1];
                int32_t prev_end = prev->y + prev->h;
                int32_t row_y = PADDING_Y + r * (int32_t)g_cell_h;
                if (prev_end == row_y &&
                    prev->x == PADDING_X +
                        g_span_min[r] * (int32_t)g_cell_w &&
                    prev->w == (g_span_max[r] - g_span_min[r] + 1) *
                        (int32_t)g_cell_w) {
                    prev->h += (int32_t)g_cell_h;
                    continue;
                }
            }

            if (n_rects == SWP_COMMIT_MAX_RECTS) {
                overflow = 1;
                break;
            }

            rects[n_rects].x = PADDING_X + g_span_min[r] * (int32_t)g_cell_w;
            rects[n_rects].y = PADDING_Y + r * (int32_t)g_cell_h;
            rects[n_rects].w = (g_span_max[r] - g_span_min[r] + 1) *
                               (int32_t)g_cell_w;
            rects[n_rects].h = (int32_t)g_cell_h;
            n_rects++;
        }

        if (overflow) {
            for (int r = 0; r < g_term.rows; r++) {
                render_span(s, r, 0, g_term.cols - 1, drawn_cursor);
            }
        }
    }

    stlxgfx_destroy_surface(s);

    if (full || overflow) {
        stlxwin_commit(win, buf, NULL, 0, 0);
    } else {
        stlxwin_commit(win, buf, rects, n_rects, 0);
    }

    /* This frame becomes the reference: grid shadow, selection, cursor
     * spot, and the spans the other buffer of the pair now lacks */
    memcpy(g_prev_cells, g_term.cells, sizeof(g_prev_cells));
    memcpy(g_prev_attrs, g_term.attrs, sizeof(g_prev_attrs));
    for (int r = 0; r < TERM_MAX_ROWS; r++) {
        int from = 0;
        int to = 0;
        if (r < g_term.rows) {
            selected_cells(r, &from, &to);
        }

        g_prev_sel_from[r] = (int16_t)from;
        g_prev_sel_to[r] = (int16_t)to;
    }

    g_prev_sel_focused = g_focused;
    g_prev_valid = 1;
    ground_buffer(buf->pixels);

    int other = 1 - grounded_index(buf->pixels);
    if (full || overflow) {
        for (int r = 0; r < g_term.rows; r++) {
            diff_min[r] = 0;
            diff_max[r] = (int16_t)(g_term.cols - 1);
        }
        for (int r = g_term.rows; r < TERM_MAX_ROWS; r++) {
            diff_max[r] = -1;
        }
    }
    stale_add(other, diff_min, diff_max);

    g_last_cur_row = drawn_cursor ? g_term.cursor_row : -1;
    g_last_cur_col = g_term.cursor_col < g_term.cols
                   ? g_term.cursor_col : g_term.cols - 1;
    g_last_buf_w = buf->width;
    g_last_buf_h = buf->height;

    return 0;
}

/* Room for len more bytes at the end of the input queue, or NULL */
static char* input_reserve(size_t len) {
    if (g_input_end + len > g_input_cap && g_input_start > 0) {
        memmove(g_input, g_input + g_input_start, g_input_end - g_input_start);
        g_input_end -= g_input_start;
        g_input_start = 0;
    }

    if (g_input_end + len > g_input_cap) {
        size_t cap = g_input_cap ? g_input_cap : 256;
        while (cap < g_input_end + len) {
            cap *= 2;
        }

        char* grown = realloc(g_input, cap);
        if (!grown) {
            return NULL;
        }

        g_input = grown;
        g_input_cap = cap;
    }

    return g_input + g_input_end;
}

/* Writes queued input as far as the pty takes it, at most INPUT_WRITE_MAX bytes a call */
static void flush_input(int master_fd) {
    size_t queued = g_input_end - g_input_start;
    if (queued == 0) {
        return;
    }

    ssize_t n = write(master_fd, g_input + g_input_start, queued < INPUT_WRITE_MAX ? queued : INPUT_WRITE_MAX);
    if (n > 0) {
        g_input_start += (size_t)n;
        g_input_taken += (uint64_t)n;
    } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
        g_input_taken += g_input_end - g_input_start;
        g_input_start = g_input_end;
    }

    if (g_input_start == g_input_end) {
        g_input_start = 0;
        g_input_end = 0;
    }
}

static void send_input(int master_fd, const char* data, size_t len) {
    char* room = input_reserve(len);
    if (!room) {
        return;
    }

    memcpy(room, data, len);
    g_input_end += len;
    flush_input(master_fd);
}

/* Drops the newest paste's text the pty has not taken, keeping the end
 * marker so a program in bracketed paste sees the paste end */
static void drop_unsent_paste(void) {
    uint64_t from = g_paste_text_from > g_input_taken ? g_paste_text_from : g_input_taken;
    if (from >= g_paste_text_to) {
        return;
    }

    size_t at = g_input_start + (size_t)(from - g_input_taken);
    size_t count = (size_t)(g_paste_text_to - from);
    memmove(g_input + at, g_input + at + count, g_input_end - at - count);
    g_input_end -= count;
    g_paste_text_to = from;
}

/* Translated characters come from the display manager, escape
 * sequences from the local special-key table, control codes from the
 * modifier bit. Returns 1 when the key sent input. */
static int handle_key(int master_fd, uint32_t ch, uint16_t usage,
                      uint8_t modifiers) {
    char seq[8];
    int len = keymap_translate(usage, g_term.app_cursor_keys,
                               seq, sizeof(seq));
    if (len > 0) {
        send_input(master_fd, seq, (size_t)len);
        return 1;
    }

    if (ch == 0 || ch > 0x7E) {
        return 0;
    }

    char byte = (char)ch;
    uint32_t low = ch | 32u;
    if ((modifiers & STLXWIN_MOD_CTRL) && low >= 'a' && low <= 'z') {
        byte = (char)(low & 0x1F);
    }

    /* Ctrl C stops a paste still being sent */
    if (byte == CTRL_C_BYTE) {
        drop_unsent_paste();
    }

    send_input(master_fd, &byte, 1);
    return 1;
}

static void copy_selection(stlxwin_conn* conn) {
    size_t len = 0;
    char* text = select_copy(&g_select, &g_term, &len);
    if (text && len > 0 && stlxwin_clipboard_set(conn, text, len) < 0) {
        printf("stlxterm: cannot copy %zu bytes to the clipboard\r\n", len);
    }

    free(text);
}

/* The clipboard goes to the program as if typed, bracketed when it
 * asked. Returns 1 when there was text to send. */
static int paste_clipboard(stlxwin_conn* conn, int master_fd) {
    char* text = NULL;
    long len = stlxwin_clipboard_get(conn, &text);
    char* room = len > 0 ? input_reserve((size_t)len + 2 * PASTE_MARKER_LEN) : NULL;
    if (!room) {
        free(text);
        return 0;
    }

    uint64_t queued_end = g_input_taken + (g_input_end - g_input_start);
    size_t encoded = paste_encode(text, (size_t)len, g_term.bracketed_paste, room);
    g_input_end += encoded;
    free(text);

    /* A paste queued behind one still being sent joins its range, so Ctrl C stops both */
    size_t marker_len = g_term.bracketed_paste ? PASTE_MARKER_LEN : 0;
    if (g_paste_text_to <= g_input_taken) {
        g_paste_text_from = queued_end + marker_len;
    }

    g_paste_text_to = queued_end + encoded - marker_len;

    flush_input(master_fd);
    return 1;
}

/* The terminal's own keys: copy and paste, each once per press, and the
 * Ctrl+Shift letters, which never reach the program as Ctrl letters.
 * Returns 1 when the key sent input. */
static int handle_terminal_key(stlxwin_conn* conn, int master_fd, const stlxwin_event* ev,
                               int* is_terminal_key) {
    uint8_t mods = ev->key.modifiers & (STLXWIN_MOD_SHIFT | STLXWIN_MOD_CTRL | STLXWIN_MOD_ALT | STLXWIN_MOD_SUPER);
    uint8_t ctrl_shift = STLXWIN_MOD_CTRL | STLXWIN_MOD_SHIFT;
    uint16_t usage = ev->key.usage;

    int is_copy = (usage == HID_USAGE_C && mods == ctrl_shift) ||
                  (usage == HID_USAGE_INSERT && mods == STLXWIN_MOD_CTRL);
    int is_paste = (usage == HID_USAGE_V && mods == ctrl_shift) ||
                   (usage == HID_USAGE_INSERT && mods == STLXWIN_MOD_SHIFT);
    int is_chord = (mods & ctrl_shift) == ctrl_shift && usage >= HID_USAGE_A && usage <= HID_USAGE_Z;
    *is_terminal_key = is_copy || is_paste || is_chord;
    if (ev->type != STLXWIN_EVT_KEY_DOWN) {
        return 0;
    }

    if (is_copy) {
        copy_selection(conn);
        return 0;
    }

    return is_paste && paste_clipboard(conn, master_fd);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The grid under a window position, held to the grid: above it is the
 * start of the first row and below it the end of the last */
static select_point_t grid_point(int32_t x, int32_t y) {
    select_point_t p = { 0, 0, 0 };
    int32_t gx = x - PADDING_X;
    int32_t gy = y - PADDING_Y;
    if (gy < 0) {
        return p;
    }

    p.row = gy / (int32_t)g_cell_h;
    if (p.row >= g_term.rows) {
        p.row = g_term.rows - 1;
        p.cell = g_term.cols - 1;
        p.edge = g_term.cols;
        return p;
    }

    if (gx > 0) {
        p.cell = gx / (int32_t)g_cell_w;
        p.edge = (gx + (int32_t)g_cell_w / 2) / (int32_t)g_cell_w;
    }

    if (p.cell >= g_term.cols) {
        p.cell = g_term.cols - 1;
    }

    if (p.edge > g_term.cols) {
        p.edge = g_term.cols;
    }

    return p;
}

/* A left press starts a selection by cells, a double click by words and
 * a triple click by lines, and Shift extends the current one instead */
static void press_select(int32_t x, int32_t y) {
    uint64_t now = now_ms();
    int is_repeat = g_clicks > 0 && now - g_last_press_ms <= MULTI_CLICK_MS &&
                    abs(x - g_last_press_x) <= MULTI_CLICK_SLOP && abs(y - g_last_press_y) <= MULTI_CLICK_SLOP;
    g_clicks = is_repeat ? g_clicks % 3 + 1 : 1;
    g_last_press_ms = now;
    g_last_press_x = x;
    g_last_press_y = y;

    select_point_t p = grid_point(x, y);
    if (g_clicks == 1 && (g_modifiers & STLXWIN_MOD_SHIFT) && select_has_text(&g_select)) {
        select_extend(&g_select, &g_term, p);
    } else {
        select_unit_t unit = g_clicks == 1 ? SELECT_BY_CELL : g_clicks == 2 ? SELECT_BY_WORD : SELECT_BY_LINE;
        select_begin(&g_select, &g_term, p, unit);
    }

    g_selecting = 1;
}

/* The left button selects: a press starts or extends a selection,
 * motion drags it and the release ends it. Returns 1 when it changed. */
static int handle_selection_pointer(const stlxwin_event* ev) {
    if (ev->type == STLXWIN_EVT_POINTER_MOTION) {
        return g_selecting && select_drag(&g_select, &g_term, grid_point(ev->motion.x, ev->motion.y));
    }

    if (ev->button.button != STLXWIN_BTN_LEFT) {
        return 0;
    }

    if (ev->type == STLXWIN_EVT_BUTTON_DOWN) {
        press_select(ev->button.x, ev->button.y);
        return 1;
    }

    if (!g_selecting) {
        return 0;
    }

    select_drag(&g_select, &g_term, grid_point(ev->button.x, ev->button.y));
    g_selecting = 0;
    if (!select_has_text(&g_select)) {
        select_clear(&g_select);
    }

    return 1;
}

/* Runs `program` with `args` on a fresh pty, whose master end goes to
 * `out_master`. Logs the cause when it fails. */
static int spawn_program(const char* program, const char* args[], int* out_master) {
    int master_fd, slave_fd;
    if (pty_create(&master_fd, &slave_fd) < 0) {
        printf("stlxterm: no pty for %s\r\n", program);
        return -1;
    }

    ioctl(slave_fd, STLX_TCSETS_RAW, 0);
    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    struct winsize ws = { (unsigned short)g_term.rows,
                          (unsigned short)g_term.cols, 0, 0 };
    ioctl(master_fd, TIOCSWINSZ, &ws);

    /* Declare the escape dialect this terminal implements */
    setenv("TERM", "xterm", 1);

    int child = proc_create(program, args);
    if (child < 0) {
        printf("stlxterm: cannot create a process for %s: %s\r\n", program, strerror(errno));
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    proc_set_handle(child, 0, slave_fd);
    proc_set_handle(child, 1, slave_fd);
    proc_set_handle(child, 2, slave_fd);
    if (proc_start(child) < 0) {
        printf("stlxterm: cannot start %s: %s\r\n", program, strerror(errno));
        close(child);
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    close(slave_fd);

    *out_master = master_fd;
    return 0;
}

/* `stlxterm <program> [args]` runs the program in place of the shell */
int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    stlxwin_conn* conn = stlxwin_connect("stlxterm");
    if (!conn) {
        printf("stlxterm: no display manager\r\n");
        return 1;
    }

    stlxwin_window* win = stlxwin_window_create(
        conn, INITIAL_WIDTH, INITIAL_HEIGHT, "stlxterm",
        STLXWIN_WF_RESIZABLE | STLXWIN_WF_GLASS | STLXWIN_WF_PREMULTIPLIED);
    if (!win) {
        printf("stlxterm: window creation failed\r\n");
        stlxwin_disconnect(conn);
        return 1;
    }

    g_font = stlxgfx_font_open(STLXTHEME_MONO_FONT_PATH, STLXTHEME_TERMINAL_SIZE);
    if (!g_font) {
        printf("stlxterm: font load failed\r\n");
        stlxwin_window_destroy(win);
        stlxwin_disconnect(conn);
        return 1;
    }

    /* A cell is one advance wide and at least the theme's line height
     * tall, with the glyphs centered in the row */
    stlxgfx_font_metrics fm;
    stlxgfx_font_metrics_get(g_font, &fm);
    g_cell_w = fm.advance_mono > 0 ? (uint32_t)fm.advance_mono : 8;
    g_cell_h = fm.line_height > STLXTHEME_TERMINAL_LINE_HEIGHT ? (uint32_t)fm.line_height
                                                               : STLXTHEME_TERMINAL_LINE_HEIGHT;
    g_ascent = fm.ascent + ((int32_t)g_cell_h - fm.line_height) / 2;
    stlxwin_window_set_min_size(win,
        2 * PADDING_X + MIN_COLS * g_cell_w,
        2 * PADDING_Y + MIN_ROWS * g_cell_h);

    int cols = (int)((INITIAL_WIDTH - 2 * PADDING_X) / g_cell_w);
    int rows = (int)((INITIAL_HEIGHT - 2 * PADDING_Y) / g_cell_h);
    term_init(&g_term, rows, cols);

    const char* program = argc > 1 ? argv[1] : SHELL_PATH;
    const char** args = argc > 2 ? (const char**)&argv[2] : NULL;
    int master_fd = -1;
    if (spawn_program(program, args, &master_fd) < 0) {
        stlxwin_window_destroy(win);
        stlxwin_disconnect(conn);
        return 1;
    }

    printf("stlxterm: %dx%d cells\r\n", g_term.cols, g_term.rows);

    int running = 1;
    int cursor_on = 1;
    int dirty = 1;

    while (running) {
        if (dirty) {
            if (render_frame(win, cursor_on) < 0) {
                break;
            }
            dirty = 0;
        }

        short pty_events = g_input_end > g_input_start ? POLLIN | POLLOUT : POLLIN;
        struct pollfd fds[2] = {
            { stlxwin_conn_fd(conn), POLLIN, 0 },
            { master_fd, pty_events, 0 },
        };

        int timeout = g_focused && g_term.cursor_visible ? BLINK_MS : -1;
        int rc = poll(fds, 2, timeout);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (rc == 0) {
            cursor_on = !cursor_on;
            dirty = 1;
            continue;
        }

        /* Drain the pty with a bound so a flood of output cannot
         * starve input handling */
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char pty_buf[4096];
            for (int i = 0; i < PTY_DRAIN_MAX; i++) {
                ssize_t n = read(master_fd, pty_buf, sizeof(pty_buf));
                if (n > 0) {
                    term_feed(&g_term, pty_buf, (int)n);
                    dirty = 1;
                    continue;
                }
                if (n == 0) {
                    running = 0;
                }
                break;
            }

            if (dirty) {
                select_follow(&g_select, &g_term);
            }

            if (dirty && g_focused) {
                cursor_on = 1;
            }
        }

        if (fds[1].revents & POLLOUT) {
            flush_input(master_fd);
        }

        if (fds[0].revents & POLLIN) {
            stlxwin_dispatch(conn);
        }

        stlxwin_event ev;
        while (stlxwin_next_event(conn, &ev)) {
            switch (ev.type) {
            case STLXWIN_EVT_CLOSE:
            case STLXWIN_EVT_DISCONNECTED:
                running = 0;
                break;
            case STLXWIN_EVT_CONFIGURE: {
                int new_cols = (int)((ev.configure.width - 2 * PADDING_X)
                                     / g_cell_w);
                int new_rows = (int)((ev.configure.height - 2 * PADDING_Y)
                                     / g_cell_h);
                int old_rows = g_term.rows;
                int old_cols = g_term.cols;
                term_resize(&g_term, new_rows, new_cols);
                if (g_term.rows != old_rows || g_term.cols != old_cols) {
                    select_clear(&g_select);
                    g_selecting = 0;
                }

                struct winsize ws = { (unsigned short)g_term.rows,
                                      (unsigned short)g_term.cols, 0, 0 };
                ioctl(master_fd, TIOCSWINSZ, &ws);
                dirty = 1;
                break;
            }
            case STLXWIN_EVT_FOCUS_IN:
                g_focused = 1;
                cursor_on = 1;
                dirty = 1;
                break;
            case STLXWIN_EVT_FOCUS_OUT:
                g_focused = 0;
                cursor_on = 0;
                g_modifiers = 0;
                dirty = 1;
                break;
            case STLXWIN_EVT_KEY_DOWN:
            case STLXWIN_EVT_KEY_REPEAT: {
                g_modifiers = ev.key.modifiers;
                int is_terminal_key = 0;
                int sent_input = handle_terminal_key(conn, master_fd, &ev, &is_terminal_key);
                if (!is_terminal_key) {
                    sent_input = handle_key(master_fd, ev.key.ch, ev.key.usage, ev.key.modifiers);
                }

                /* A key that sends input ends the selection */
                if (sent_input && g_select.active) {
                    select_clear(&g_select);
                    dirty = 1;
                }

                if (g_focused) {
                    cursor_on = 1;
                }
                break;
            }
            case STLXWIN_EVT_KEY_UP:
                g_modifiers = ev.key.modifiers;
                break;
            case STLXWIN_EVT_BUTTON_DOWN:
            case STLXWIN_EVT_BUTTON_UP:
            case STLXWIN_EVT_POINTER_MOTION:
                dirty |= handle_selection_pointer(&ev);
                break;
            default:
                break;
            }
        }

        /* OSC title sequences surface on the window frame */
        if (g_term.title_changed) {
            stlxwin_window_set_title(win, g_term.title);
            g_term.title_changed = 0;
        }
    }

    close(master_fd);
    stlxgfx_font_close(g_font);
    stlxwin_window_destroy(win);
    stlxwin_disconnect(conn);
    return 0;
}
