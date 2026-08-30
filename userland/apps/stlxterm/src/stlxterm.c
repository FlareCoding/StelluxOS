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
#include <stlx/proc.h>
#include <stlx/pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>

#include "keymap.h"
#include "term.h"

#define STLX_TCSETS_RAW   0x7301

#define INITIAL_WIDTH     800
#define INITIAL_HEIGHT    600
#define MIN_COLS          20
#define MIN_ROWS          5
#define BG_COLOR          0xFF1E1E2E
#define CURSOR_COLOR      0xFFF5E0DC
#define FONT_SIZE         16
#define PADDING           4
#define BLINK_MS          500
#define PTY_DRAIN_MAX     16

static const uint32_t FG_PALETTE[17] = {
    0xFFCDD6F4, // 0: default foreground
    0xFF45475A, // 1: black (30)
    0xFFF38BA8, // 2: red (31)
    0xFFA6E3A1, // 3: green (32)
    0xFFF9E2AF, // 4: yellow (33)
    0xFF89B4FA, // 5: blue (34)
    0xFFF5C2E7, // 6: magenta (35)
    0xFF94E2D5, // 7: cyan (36)
    0xFFBAC2DE, // 8: white (37)
    0xFF585B70, // 9: bright black (90)
    0xFFF38BA8, // 10: bright red (91)
    0xFFA6E3A1, // 11: bright green (92)
    0xFFF9E2AF, // 12: bright yellow (93)
    0xFF89B4FA, // 13: bright blue (94)
    0xFFF5C2E7, // 14: bright magenta (95)
    0xFF94E2D5, // 15: bright cyan (96)
    0xFFA6ADC8, // 16: bright white (97)
};

static const uint32_t BG_PALETTE[17] = {
    0xFF1E1E2E, // 0: default background
    0xFF45475A, // 1: black (40)
    0xFFF38BA8, // 2: red (41)
    0xFFA6E3A1, // 3: green (42)
    0xFFF9E2AF, // 4: yellow (43)
    0xFF89B4FA, // 5: blue (44)
    0xFFF5C2E7, // 6: magenta (45)
    0xFF94E2D5, // 7: cyan (46)
    0xFFBAC2DE, // 8: white (47)
    0xFF585B70, // 9: bright black (100)
    0xFFF38BA8, // 10: bright red (101)
    0xFFA6E3A1, // 11: bright green (102)
    0xFFF9E2AF, // 12: bright yellow (103)
    0xFF89B4FA, // 13: bright blue (104)
    0xFFF5C2E7, // 14: bright magenta (105)
    0xFF94E2D5, // 15: bright cyan (106)
    0xFFA6ADC8, // 16: bright white (107)
};

static term_state_t g_term;
static uint32_t g_cell_w = 8;
static uint32_t g_cell_h = 16;

/* The grid as it was last painted, diffed against the live grid to
 * find damage. A second span set remembers the previous frame so the
 * other buffer of the pair can be brought up to date. */
static char g_prev_cells[TERM_MAX_ROWS][TERM_MAX_COLS];
static term_attr_t g_prev_attrs[TERM_MAX_ROWS][TERM_MAX_COLS];
static int g_prev_valid = 0;
static int16_t g_span_min[TERM_MAX_ROWS];
static int16_t g_span_max[TERM_MAX_ROWS];
static int16_t g_last_min[TERM_MAX_ROWS];
static int16_t g_last_max[TERM_MAX_ROWS];
static int g_last_cur_row = -1;
static int g_last_cur_col = -1;
static void* g_last_pixels = NULL;
static uint32_t g_last_buf_w = 0;
static uint32_t g_last_buf_h = 0;

static void span_mark(int row, int c0, int c1) {
    if (row < 0 || row >= g_term.rows) {
        return;
    }
    if (g_span_min[row] > c0) g_span_min[row] = (int16_t)c0;
    if (g_span_max[row] < c1) g_span_max[row] = (int16_t)c1;
}

/* Paint one run of cells, background first, glyphs on top, and the
 * cursor cell inverted when it falls inside the run */
static void render_span(stlxgfx_surface_t* s, int row, int c0, int c1,
                        int cursor_on) {
    char ch_buf[2] = { 0, 0 };

    for (int c = c0; c <= c1; c++) {
        int32_t px = PADDING + (int32_t)((uint32_t)c * g_cell_w);
        int32_t py = PADDING + (int32_t)((uint32_t)row * g_cell_h);

        int cc = g_term.cursor_col < g_term.cols
               ? g_term.cursor_col : g_term.cols - 1;
        int at_cursor = cursor_on &&
                        row == g_term.cursor_row && c == cc;

        uint32_t bg = at_cursor ? CURSOR_COLOR
                    : BG_PALETTE[g_term.attrs[row][c].bg];
        stlxgfx_fill_rect(s, px, py, g_cell_w, g_cell_h, bg);

        if (g_term.cells[row][c] != ' ') {
            uint32_t fg = at_cursor ? BG_COLOR
                        : FG_PALETTE[g_term.attrs[row][c].fg];
            ch_buf[0] = g_term.cells[row][c];
            stlxgfx_draw_text(s, px, py, ch_buf, FONT_SIZE, fg);
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

    int full = !g_prev_valid ||
               buf->width != g_last_buf_w || buf->height != g_last_buf_h;

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

        /* The other buffer of the pair is one frame behind, so the
         * previous frame's spans repaint into it as well */
        if (buf->pixels != g_last_pixels) {
            for (int r = 0; r < g_term.rows; r++) {
                if (g_last_max[r] >= 0) {
                    span_mark(r, g_last_min[r], g_last_max[r]);
                }
            }
        }
    }

    stlxgfx_surface_t* s = stlxgfx_surface_from_buffer(
        (uint8_t*)buf->pixels, buf->width, buf->height, buf->stride,
        32, 16, 8, 0);
    if (!s) {
        return -1;
    }

    int drawn_cursor = cursor_on && g_term.cursor_visible;

    stlxwin_rect rects[SWP_COMMIT_MAX_RECTS];
    uint32_t n_rects = 0;
    int overflow = 0;

    if (full) {
        stlxgfx_clear(s, BG_COLOR);
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
                int32_t row_y = PADDING + r * (int32_t)g_cell_h;
                if (prev_end == row_y &&
                    prev->x == PADDING +
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

            rects[n_rects].x = PADDING + g_span_min[r] * (int32_t)g_cell_w;
            rects[n_rects].y = PADDING + r * (int32_t)g_cell_h;
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

    /* This frame becomes the reference: grid shadow, span history,
     * cursor spot, and which buffer of the pair was painted */
    memcpy(g_prev_cells, g_term.cells, sizeof(g_prev_cells));
    memcpy(g_prev_attrs, g_term.attrs, sizeof(g_prev_attrs));
    g_prev_valid = 1;
    for (int r = 0; r < TERM_MAX_ROWS; r++) {
        if (full || overflow) {
            g_last_min[r] = 0;
            g_last_max[r] = (int16_t)(g_term.cols - 1);
        } else {
            g_last_min[r] = g_span_min[r];
            g_last_max[r] = g_span_max[r];
        }
    }
    g_last_cur_row = drawn_cursor ? g_term.cursor_row : -1;
    g_last_cur_col = g_term.cursor_col < g_term.cols
                   ? g_term.cursor_col : g_term.cols - 1;
    g_last_pixels = buf->pixels;
    g_last_buf_w = buf->width;
    g_last_buf_h = buf->height;

    return 0;
}

static void write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return;
        }

        data += n;
        len -= (size_t)n;
    }
}

/* Translated characters come from the display manager, escape
 * sequences from the local special-key table, control codes from the
 * modifier bit. */
static void handle_key(int master_fd, uint32_t ch, uint16_t usage,
                       uint8_t modifiers) {
    char seq[8];
    int len = keymap_translate(usage, g_term.app_cursor_keys,
                               seq, sizeof(seq));
    if (len > 0) {
        write_all(master_fd, seq, (size_t)len);
        return;
    }

    if (ch == 0 || ch > 0x7E) {
        return;
    }

    char byte = (char)ch;
    uint32_t low = ch | 32u;
    if ((modifiers & STLXWIN_MOD_CTRL) && low >= 'a' && low <= 'z') {
        byte = (char)(low & 0x1F);
    }

    write_all(master_fd, &byte, 1);
}

static int spawn_shell(int* out_master) {
    int master_fd, slave_fd;
    if (pty_create(&master_fd, &slave_fd) < 0) {
        return -1;
    }

    ioctl(slave_fd, STLX_TCSETS_RAW, 0);
    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    struct winsize ws = { (unsigned short)g_term.rows,
                          (unsigned short)g_term.cols, 0, 0 };
    ioctl(master_fd, TIOCSWINSZ, &ws);

    /* Declare the escape dialect this terminal implements */
    setenv("TERM", "xterm", 1);

    int shell = proc_create("/bin/shell", NULL);
    if (shell < 0) {
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    proc_set_handle(shell, 0, slave_fd);
    proc_set_handle(shell, 1, slave_fd);
    proc_set_handle(shell, 2, slave_fd);
    if (proc_start(shell) < 0) {
        close(shell);
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    close(slave_fd);

    *out_master = master_fd;
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    stlxwin_conn* conn = stlxwin_connect("stlxterm");
    if (!conn) {
        printf("stlxterm: no display manager\r\n");
        return 1;
    }

    stlxwin_window* win = stlxwin_window_create(
        conn, INITIAL_WIDTH, INITIAL_HEIGHT, "stlxterm",
        STLXWIN_WF_RESIZABLE);
    if (!win) {
        printf("stlxterm: window creation failed\r\n");
        stlxwin_disconnect(conn);
        return 1;
    }

    stlxgfx_text_size("M", FONT_SIZE, &g_cell_w, &g_cell_h);
    if (g_cell_w == 0) g_cell_w = 8;
    if (g_cell_h == 0) g_cell_h = 16;
    stlxwin_window_set_min_size(win,
        2 * PADDING + MIN_COLS * g_cell_w,
        2 * PADDING + MIN_ROWS * g_cell_h);

    int cols = (int)((INITIAL_WIDTH - 2 * PADDING) / g_cell_w);
    int rows = (int)((INITIAL_HEIGHT - 2 * PADDING) / g_cell_h);
    term_init(&g_term, rows, cols);

    int master_fd = -1;
    if (spawn_shell(&master_fd) < 0) {
        printf("stlxterm: failed to start the shell\r\n");
        stlxwin_window_destroy(win);
        stlxwin_disconnect(conn);
        return 1;
    }

    printf("stlxterm: %dx%d cells\r\n", g_term.cols, g_term.rows);

    int running = 1;
    int focused = 1;
    int cursor_on = 1;
    int dirty = 1;

    while (running) {
        if (dirty) {
            if (render_frame(win, cursor_on) < 0) {
                break;
            }
            dirty = 0;
        }

        struct pollfd fds[2] = {
            { stlxwin_conn_fd(conn), POLLIN, 0 },
            { master_fd, POLLIN, 0 },
        };

        int timeout = focused && g_term.cursor_visible ? BLINK_MS : -1;
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

            if (dirty && focused) {
                cursor_on = 1;
            }
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
                int new_cols = (int)((ev.configure.width - 2 * PADDING)
                                     / g_cell_w);
                int new_rows = (int)((ev.configure.height - 2 * PADDING)
                                     / g_cell_h);
                term_resize(&g_term, new_rows, new_cols);

                struct winsize ws = { (unsigned short)g_term.rows,
                                      (unsigned short)g_term.cols, 0, 0 };
                ioctl(master_fd, TIOCSWINSZ, &ws);
                dirty = 1;
                break;
            }
            case STLXWIN_EVT_FOCUS_IN:
                focused = 1;
                cursor_on = 1;
                dirty = 1;
                break;
            case STLXWIN_EVT_FOCUS_OUT:
                focused = 0;
                cursor_on = 0;
                dirty = 1;
                break;
            case STLXWIN_EVT_KEY_DOWN:
            case STLXWIN_EVT_KEY_REPEAT:
                handle_key(master_fd, ev.key.ch, ev.key.usage,
                           ev.key.modifiers);
                if (focused) {
                    cursor_on = 1;
                }
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
    stlxwin_window_destroy(win);
    stlxwin_disconnect(conn);
    return 0;
}
