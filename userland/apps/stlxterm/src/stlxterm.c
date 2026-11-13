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
#include <sys/stat.h>

#include "config.h"
#include "drop.h"
#include "keymap.h"
#include "overlay.h"
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
#define CWD_OPTION        "--cwd"

/* Presses this close in time and place count as one double or triple click, Qt's defaults */
#define MULTI_CLICK_MS    400
#define MULTI_CLICK_SLOP  5

/* The echo of one write must fit the pty's output buffer, which the loop drains between writes */
#define INPUT_WRITE_MAX   2048

/* How long the copied pill and a drop's note stay, and the widths of the paste questions */
#define COPIED_PILL_MS    1500
#define NOTE_PILL_MS      3000
#define LINES_QUESTION_W  520
#define SIZE_QUESTION_W   440

#define HID_USAGE_A       0x04
#define HID_USAGE_C       0x06
#define HID_USAGE_V       0x19
#define HID_USAGE_Z       0x1D
#define HID_USAGE_ENTER   0x28
#define HID_USAGE_ESCAPE  0x29
#define HID_USAGE_TAB     0x2B
#define HID_USAGE_INSERT  0x49
#define HID_USAGE_RIGHT   0x4F
#define HID_USAGE_LEFT    0x50
#define HID_USAGE_KP_ENTER 0x58
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

typedef enum {
    SELECTION_UNCHANGED,
    SELECTION_CHANGED,
    SELECTION_FINISHED, // a release left text selected
} selection_change_t;

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

/* The settings file as last read, and the program a paste question names */
static term_config_t g_config;
static struct stat g_config_stat;
static const char* g_program_name = "shell";

/* A paste a question holds back, a malloc'd copy or the clipboard's own mapping, the pill at the bottom, which confirms
 * a copy or says what a drop left out, and when it leaves, 0 while none shows, and whether the last frame drew any
 * overlay */
static const char* g_held_paste = NULL;
static size_t g_held_paste_len = 0;
static int g_is_held_mapped = 0;
static overlay_question_t g_question;
static char g_pill_text[OVERLAY_TEXT_MAX];
static int g_is_copied_pill = 0;
static uint64_t g_pill_until_ms = 0;
static int g_overlay_drawn = 0;

/* The drag over the window, lit while the terminal would paste it: what it pastes, the type read for it, and the words
 * on its chip */
static drop_kind_t g_drop_kind = DROP_REFUSED;
static const char* g_drop_mime = NULL;
static char g_drop_words[STLXWIN_DROP_WORDS_MAX];

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
    int has_overlay = g_held_paste || g_pill_until_ms || g_drop_kind != DROP_REFUSED;
    int touches_overlay = has_overlay || g_overlay_drawn;
    int full = !g_prev_valid || resized || grounded_index(buf->pixels) < 0 || touches_overlay;

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

        if (g_drop_kind != DROP_REFUSED) {
            overlay_draw_drop_target(s);
        }

        if (g_pill_until_ms && g_is_copied_pill) {
            overlay_draw_copied(s, g_pill_text);
        } else if (g_pill_until_ms) {
            overlay_draw_note(s, g_pill_text);
        }

        if (g_held_paste) {
            overlay_draw_question(s, &g_question);
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
    g_overlay_drawn = has_overlay;
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

    /* An overlay reaches into the padding, which the spans owed to the other buffer never cover, so it paints whole */
    if (touches_overlay) {
        g_grounded[other] = NULL;
    }

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
    int len = keymap_translate(usage, modifiers, g_term.app_cursor_keys,
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

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* A count with its thousands set apart by commas, such as 61,204 */
static void format_count(size_t count, char* out, size_t cap) {
    char digits[32];
    int len = snprintf(digits, sizeof(digits), "%zu", count);
    size_t n = 0;
    for (int i = 0; i < len && n + 2 < cap; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            out[n++] = ',';
        }

        out[n++] = digits[i];
    }

    out[n] = '\0';
}

/* Shows the pill for `ms`, saying Copied and `text` when it confirms a copy, else `text` alone */
static void show_pill(const char* text, int is_copied, uint64_t ms) {
    snprintf(g_pill_text, sizeof(g_pill_text), "%s", text);
    g_is_copied_pill = is_copied;
    g_pill_until_ms = now_ms() + ms;
}

/* The selection goes to the clipboard, and the copied pill says how
 * much: lines when it holds a line break, otherwise characters */
static void copy_selection(stlxwin_conn* conn) {
    size_t len = 0;
    char* text = select_copy(&g_select, &g_term, &len);
    if (!text || len == 0) {
        free(text);
        return;
    }

    if (stlxwin_clipboard_set(conn, text, len) < 0) {
        printf("stlxterm: cannot copy %zu bytes to the clipboard\r\n", len);
        free(text);
        return;
    }

    int has_line_break = memchr(text, '\n', len) != NULL;
    size_t count = has_line_break ? paste_line_count(text, len) : len;
    const char* unit = has_line_break ? (count == 1 ? "line" : "lines") : (count == 1 ? "character" : "characters");
    free(text);

    char figure[32];
    char amount[OVERLAY_TEXT_MAX];
    format_count(count, figure, sizeof(figure));
    snprintf(amount, sizeof(amount), "%s %s", figure, unit);
    show_pill(amount, 1, COPIED_PILL_MS);
}

/* Queues pasted text for the program as if typed, bracketed when it asked */
static void queue_paste(int master_fd, const char* text, size_t len) {
    char* room = input_reserve(len + 2 * PASTE_MARKER_LEN);
    if (!room) {
        return;
    }

    uint64_t queued_end = g_input_taken + (g_input_end - g_input_start);
    size_t encoded = paste_encode(text, len, g_term.bracketed_paste, room);
    g_input_end += encoded;

    /* A paste queued behind one still being sent joins its range, so Ctrl C stops both */
    size_t marker_len = g_term.bracketed_paste ? PASTE_MARKER_LEN : 0;
    if (g_paste_text_to <= g_input_taken) {
        g_paste_text_from = queued_end + marker_len;
    }

    g_paste_text_to = queued_end + encoded - marker_len;
    flush_input(master_fd);
}

/* A paste question's first lines, control characters dropped and tabs
 * shown as spaces, and how many lines it leaves out */
static void fill_preview(overlay_question_t* q, const char* text, size_t len, size_t lines) {
    const char* p = text;
    const char* end = text + len;
    while (p < end && q->preview_count < OVERLAY_PREVIEW_LINES) {
        char* out = q->preview[q->preview_count++];
        size_t n = 0;
        for (; p < end && *p != '\n' && *p != '\r'; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '\t' && n + 4 < OVERLAY_TEXT_MAX) {
                memcpy(out + n, "    ", 4);
                n += 4;
            } else if (c >= 0x20 && c != 0x7F && n + 1 < OVERLAY_TEXT_MAX) {
                out[n++] = (char)c;
            }
        }

        out[n] = '\0';
        if (p < end) {
            p += p + 1 < end && p[0] == '\r' && p[1] == '\n' ? 2 : 1;
        }
    }

    if (lines > OVERLAY_PREVIEW_LINES) {
        char count[32];
        format_count(lines - OVERLAY_PREVIEW_LINES, count, sizeof(count));
        snprintf(q->preview_more, sizeof(q->preview_more), "and %s more %s", count,
                 lines - OVERLAY_PREVIEW_LINES == 1 ? "line" : "lines");
    }
}

/* A paste's text, which a malloc'd copy frees and the clipboard's mapping unmaps */
static void release_paste(const char* text, size_t len, int is_mapped) {
    if (is_mapped) {
        stlxwin_clipboard_unmap(text, len);
    } else {
        free((char*)text);
    }
}

/* Holds a paste behind its question, which takes ownership of text */
static void ask_before_paste(paste_check_t check, const char* text, size_t len, int is_mapped) {
    overlay_question_t* q = &g_question;
    memset(q, 0, sizeof(*q));

    size_t lines = paste_line_count(text, len);
    char count[32];
    format_count(lines, count, sizeof(count));
    const char* noun = lines == 1 ? "line" : "lines";
    if (check == PASTE_ASK_SIZE) {
        q->width = SIZE_QUESTION_W;
        snprintf(q->title, sizeof(q->title), "Paste %.1f MB?", (double)len / 1e6);
        snprintf(q->body, sizeof(q->body),
                 "It holds %s %s and takes a while to send. The window stays live, and Ctrl C stops it.", count, noun);
        snprintf(q->accept, sizeof(q->accept), "Paste");
    } else {
        q->width = LINES_QUESTION_W;
        snprintf(q->title, sizeof(q->title), "Paste %s %s into %s?", count, noun, g_program_name);
        snprintf(q->body, sizeof(q->body), "%s hasn't turned on bracketed paste, so each line runs the moment it arrives.",
                 g_program_name);
        fill_preview(q, text, len, lines);
        snprintf(q->note, sizeof(q->note), "Control characters are removed either way");
        snprintf(q->accept, sizeof(q->accept), "Paste %s %s", count, lines == 1 ? "Line" : "Lines");
    }

    snprintf(q->cancel, sizeof(q->cancel), "Cancel");
    q->is_accept_focused = 1;
    g_held_paste = text;
    g_held_paste_len = len;
    g_is_held_mapped = is_mapped;

    /* The question takes the pointer, so a drag under way stops where it is */
    g_selecting = 0;
}

/* Sends or drops the held paste, as its question was answered. Returns 1 when it sent input. */
static int answer_question(int master_fd, int accepts) {
    if (accepts) {
        queue_paste(master_fd, g_held_paste, g_held_paste_len);
    }

    release_paste(g_held_paste, g_held_paste_len, g_is_held_mapped);
    g_held_paste = NULL;
    g_held_paste_len = 0;
    return accepts;
}

/* Enter takes the focused button, Escape cancels, Tab moves the focus
 * and the arrows put it on their side. Returns 1 when it sent input. */
static int handle_question_key(int master_fd, const stlxwin_event* ev) {
    if (ev->type != STLXWIN_EVT_KEY_DOWN) {
        return 0;
    }

    switch (ev->key.usage) {
    case HID_USAGE_ENTER:
    case HID_USAGE_KP_ENTER:
        return answer_question(master_fd, g_question.is_accept_focused);
    case HID_USAGE_ESCAPE:
        return answer_question(master_fd, 0);
    case HID_USAGE_TAB:
        g_question.is_accept_focused = !g_question.is_accept_focused;
        return 0;
    case HID_USAGE_LEFT:
    case HID_USAGE_RIGHT:
        g_question.is_accept_focused = ev->key.usage == HID_USAGE_RIGHT;
        return 0;
    default:
        return 0;
    }
}

/* A left press on a question's button answers it. Returns 1 when it did. */
static int handle_question_pointer(int master_fd, const stlxwin_event* ev) {
    if (ev->type != STLXWIN_EVT_BUTTON_DOWN || ev->button.button != STLXWIN_BTN_LEFT) {
        return 0;
    }

    overlay_hit_t hit = overlay_question_hit(ev->button.x, ev->button.y);
    if (hit == OVERLAY_HIT_NONE) {
        return 0;
    }

    answer_question(master_fd, hit == OVERLAY_HIT_ACCEPT);
    return 1;
}

/* Pasted text goes to the program, or waits behind a question about its lines or its size, and is taken either way.
 * Returns 1 when it went straight to the program. */
static int paste_text(int master_fd, const char* text, size_t len, int is_mapped) {
    paste_check_t check = paste_check(text, len, g_term.bracketed_paste, g_config.paste_protection);
    if (check != PASTE_SEND) {
        ask_before_paste(check, text, len, is_mapped);
        return 0;
    }

    queue_paste(master_fd, text, len);
    release_paste(text, len, is_mapped);
    return 1;
}

/* The pill says why files were left out of a drop or a paste, another computer before a name the shell would misread */
static void note_left_out(const drop_paste_t* paste) {
    char note[OVERLAY_TEXT_MAX];
    if (paste->remote == 1) {
        snprintf(note, sizeof(note), "This file is on another computer (%s)", paste->host);
    } else if (paste->remote > 1) {
        snprintf(note, sizeof(note), "%u files are on another computer (%s)", (unsigned)paste->remote, paste->host);
    } else if (paste->unsafe == 1) {
        snprintf(note, sizeof(note), "A name holds a line break, so it was left out");
    } else if (paste->unsafe > 1) {
        snprintf(note, sizeof(note), "%u names hold line breaks, so they were left out", (unsigned)paste->unsafe);
    } else {
        return;
    }

    show_pill(note, 0, NOTE_PILL_MS);
}

/* A paste with no text says what the clipboard holds instead, which watching the clipboard tells from then on */
static void note_textless_paste(stlxwin_conn* conn) {
    const char* note = "The clipboard holds no text";
    stlxwin_offer_types types;
    if (stlxwin_clipboard_watch(conn) == 0 && stlxwin_clipboard_types(conn, &types) == 0) {
        int is_image = 0;
        for (uint32_t i = 0; i < types.count; i++) {
            is_image |= strncmp(types.types[i], "image/", strlen("image/")) == 0;
        }

        note = types.count == 0 ? "The clipboard is empty" : is_image ? "The clipboard holds an image" : note;
    }

    show_pill(note, 0, NOTE_PILL_MS);
}

/* A copied file pastes as its quoted path, as a drop does, leaving out a name the shell would misread, and anything
 * without a URI list as its text. Returns 1 when something pasted. */
static int paste_clipboard(stlxwin_conn* conn, int master_fd) {
    char* list = NULL;
    long list_len = stlxwin_clipboard_get_type(conn, "text/uri-list", &list);
    drop_paste_t paste = { 0 };
    int rc = list_len > 0 ? drop_paste_uri_list(list, (size_t)list_len, &paste) : -1;
    free(list);
    if (rc == 0 && paste.pasted + paste.remote + paste.unsafe > 0) {
        note_left_out(&paste);
        if (paste.pasted > 0) {
            queue_paste(master_fd, paste.text, paste.len);
        }

        free(paste.text);
        return paste.pasted > 0;
    }

    free(paste.text);

    /* The text is read in place, so a paste large enough to ask about costs nothing until it is sent */
    const void* text = NULL;
    long len = stlxwin_clipboard_map_type(conn, STLXWIN_MIME_TEXT, &text);
    if (len <= 0) {
        if (len == 0) {
            note_textless_paste(conn);
        }

        return 0;
    }

    return paste_text(master_fd, text, (size_t)len, 1);
}

/* The terminal pastes a drag's files or text as a copy, and refuses anything else and any drag while a question holds
 * a paste */
static void answer_drag(stlxwin_window* win, const stlxwin_event* ev) {
    stlxwin_offer_types types;
    if (ev->type == STLXWIN_EVT_DROP_ENTER) {
        int is_readable = !g_held_paste && stlxwin_drop_types(win, &types) == 0;
        g_drop_kind = is_readable ? drop_kind(&types, &g_drop_mime) : DROP_REFUSED;
        if (g_drop_kind == DROP_FILES && ev->drop.item_count > 1) {
            snprintf(g_drop_words, sizeof(g_drop_words), "Paste %u paths", (unsigned)ev->drop.item_count);
        } else {
            snprintf(g_drop_words, sizeof(g_drop_words), "%s", g_drop_kind == DROP_FILES ? "Paste path" : "Paste text");
        }
    }

    if (g_drop_kind == DROP_REFUSED) {
        stlxwin_drop_status(win, 0, 0, 0, NULL);
        return;
    }

    stlxwin_drop_status(win, STLXWIN_DND_COPY, STLXWIN_DND_COPY, STLXWIN_DND_COPY, g_drop_words);
}

/* A drop pastes its files' quoted paths, which hold no line break to ask about, or its text as Ctrl Shift V does.
 * Returns the STLXWIN_DND_* action done. */
static uint32_t paste_drop(stlxwin_window* win, int master_fd) {
    char* data = NULL;
    long len = g_drop_kind != DROP_REFUSED ? stlxwin_drop_read(win, g_drop_mime, &data) : 0;
    if (len <= 0) {
        free(data);
        return 0;
    }

    if (g_drop_kind == DROP_TEXT) {
        paste_text(master_fd, data, (size_t)len, 0);
        return STLXWIN_DND_COPY;
    }

    drop_paste_t paste;
    int rc = drop_paste_uri_list(data, (size_t)len, &paste);
    free(data);
    if (rc != 0) {
        return 0;
    }

    note_left_out(&paste);
    if (paste.pasted > 0) {
        queue_paste(master_fd, paste.text, paste.len);
    }

    free(paste.text);
    return paste.pasted > 0 ? STLXWIN_DND_COPY : 0;
}

/* The settings file is read again when a window gains focus after it changed */
static void reload_config(void) {
    struct stat now;
    if (stat(CONFIG_PATH, &now) != 0) {
        memset(&now, 0, sizeof(now));
    }

    int is_unchanged = now.st_mtim.tv_sec == g_config_stat.st_mtim.tv_sec &&
                       now.st_mtim.tv_nsec == g_config_stat.st_mtim.tv_nsec && now.st_size == g_config_stat.st_size;
    if (is_unchanged) {
        return;
    }

    config_load(&g_config, CONFIG_PATH);
    g_config_stat = now;
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
 * motion drags it and the release ends it */
static selection_change_t handle_selection_pointer(const stlxwin_event* ev) {
    if (ev->type == STLXWIN_EVT_POINTER_MOTION) {
        int moved = g_selecting && select_drag(&g_select, &g_term, grid_point(ev->motion.x, ev->motion.y));
        return moved ? SELECTION_CHANGED : SELECTION_UNCHANGED;
    }

    if (ev->button.button != STLXWIN_BTN_LEFT) {
        return SELECTION_UNCHANGED;
    }

    if (ev->type == STLXWIN_EVT_BUTTON_DOWN) {
        press_select(ev->button.x, ev->button.y);
        return SELECTION_CHANGED;
    }

    if (!g_selecting) {
        return SELECTION_UNCHANGED;
    }

    select_drag(&g_select, &g_term, grid_point(ev->button.x, ev->button.y));
    g_selecting = 0;
    if (!select_has_text(&g_select)) {
        select_clear(&g_select);
        return SELECTION_CHANGED;
    }

    return SELECTION_FINISHED;
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

    /* stlxterm [--cwd folder] [program [args...]]: the program, the shell unless one is named, starts in the folder */
    int first = 1;
    if (argc > 2 && strcmp(argv[1], CWD_OPTION) == 0) {
        if (chdir(argv[2]) != 0) {
            printf("stlxterm: cannot enter %s: %s\r\n", argv[2], strerror(errno));
        }

        first = 3;
    }

    const char* program = argc > first ? argv[first] : SHELL_PATH;
    const char** args = argc > first + 1 ? (const char**)&argv[first + 1] : NULL;
    const char* program_slash = strrchr(program, '/');
    g_program_name = program_slash ? program_slash + 1 : program;
    config_load(&g_config, CONFIG_PATH);
    stat(CONFIG_PATH, &g_config_stat);

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
        if (g_pill_until_ms && now_ms() >= g_pill_until_ms) {
            g_pill_until_ms = 0;
            dirty = 1;
        }

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

        /* The cursor blinks, and the pill leaves on time, without input */
        int timeout = g_focused && g_term.cursor_visible && !g_held_paste ? BLINK_MS : -1;
        if (g_pill_until_ms) {
            uint64_t now = now_ms();
            int pill_left = g_pill_until_ms > now ? (int)(g_pill_until_ms - now) : 0;
            timeout = timeout < 0 || pill_left < timeout ? pill_left : timeout;
        }

        int rc = poll(fds, 2, timeout);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (rc == 0) {
            if (!g_pill_until_ms || now_ms() < g_pill_until_ms) {
                cursor_on = !cursor_on;
            }

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
                reload_config();
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
                int sent_input = 0;
                if (g_held_paste) {
                    sent_input = handle_question_key(master_fd, &ev);
                    dirty = 1;
                } else {
                    int is_terminal_key = 0;
                    sent_input = handle_terminal_key(conn, master_fd, &ev, &is_terminal_key);
                    if (is_terminal_key) {
                        dirty = 1;
                    } else {
                        sent_input = handle_key(master_fd, ev.key.ch, ev.key.usage, ev.key.modifiers);
                    }
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
            case STLXWIN_EVT_PASTE:
                /* The desktop's paste goes as Ctrl Shift V's does, unless a question already holds one */
                if (g_held_paste) {
                    break;
                }

                if (paste_clipboard(conn, master_fd) && g_select.active) {
                    select_clear(&g_select);
                }

                dirty = 1;
                break;
            case STLXWIN_EVT_BUTTON_DOWN:
            case STLXWIN_EVT_BUTTON_UP:
            case STLXWIN_EVT_POINTER_MOTION: {
                if (g_held_paste) {
                    dirty |= handle_question_pointer(master_fd, &ev);
                    break;
                }

                selection_change_t change = handle_selection_pointer(&ev);
                if (change == SELECTION_FINISHED && g_config.copy_on_select) {
                    copy_selection(conn);
                }

                dirty |= change != SELECTION_UNCHANGED;
                break;
            }
            case STLXWIN_EVT_DROP_ENTER:
            case STLXWIN_EVT_DROP_MOTION:
                answer_drag(win, &ev);
                dirty |= ev.type == STLXWIN_EVT_DROP_ENTER;
                break;
            case STLXWIN_EVT_DROP_LEAVE:
                g_drop_kind = DROP_REFUSED;
                dirty = 1;
                break;
            case STLXWIN_EVT_DROP: {
                /* What the drop typed lands at the cursor, so the window takes the focus for Enter to run it */
                uint32_t action = paste_drop(win, master_fd);
                stlxwin_drop_finish(win, action, action ? STLXWIN_DROP_FOCUS : 0);
                g_drop_kind = DROP_REFUSED;
                if (action && g_select.active) {
                    select_clear(&g_select);
                }

                dirty = 1;
                break;
            }
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
