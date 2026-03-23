#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/window.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>
#include <stlxgfx/event.h>
#include <stlx/pty.h>
#include <stlx/proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "keymap.h"
#include "term.h"

#define STLX_TCSETS_RAW 0x5401

/* Catppuccin Mocha palette */
#define COL_BASE       0xFF1E1E2E
#define COL_CRUST      0xFF11111B
#define COL_TEXT       0xFFCDD6F4
#define COL_ROSEWATER  0xFFF5E0DC

/* Layout */
#define WIN_WIDTH      900
#define WIN_HEIGHT     560
#define CONTENT_PAD_X  10
#define CONTENT_PAD_Y  8
#define FONT_SIZE      20
#define LINE_PAD       4

static uint32_t g_cell_w;
static uint32_t g_cell_h;

static void measure_cell(void) {
    stlxgfx_text_size("M", FONT_SIZE, &g_cell_w, &g_cell_h);
    if (g_cell_w == 0) g_cell_w = 12;
    if (g_cell_h == 0) g_cell_h = 20;
    g_cell_h += LINE_PAD;
}

static void render_term(stlxgfx_surface_t* buf, term_state* t) {
    stlxgfx_clear(buf, COL_BASE);

    int32_t origin_x = CONTENT_PAD_X;
    int32_t origin_y = CONTENT_PAD_Y;

    char ch_str[2] = {0, 0};
    for (int r = 0; r < t->rows; r++) {
        int32_t py = origin_y + (int32_t)(r * g_cell_h);
        for (int c = 0; c < t->cols; c++) {
            uint32_t fg = t->cells[r][c].fg;
            uint32_t bg = t->cells[r][c].bg;
            char ch = t->cells[r][c].ch;

            if (bg != COL_BASE) {
                int32_t px = origin_x + (int32_t)(c * g_cell_w);
                stlxgfx_fill_rect(buf, px, py, g_cell_w, g_cell_h, bg);
            }

            if (ch <= ' ') continue;
            ch_str[0] = ch;
            int32_t px = origin_x + (int32_t)(c * g_cell_w);
            stlxgfx_draw_text(buf, px, py, ch_str, FONT_SIZE, fg);
        }
    }

    int32_t cx = origin_x + (int32_t)(t->cursor_col * g_cell_w);
    int32_t cy = origin_y + (int32_t)(t->cursor_row * g_cell_h);
    stlxgfx_fill_rect(buf, cx, cy, g_cell_w, g_cell_h - LINE_PAD,
                       COL_ROSEWATER);

    char cursor_ch = t->cells[t->cursor_row][t->cursor_col].ch;
    if (cursor_ch > ' ') {
        ch_str[0] = cursor_ch;
        stlxgfx_draw_text(buf, cx, cy, ch_str, FONT_SIZE, COL_CRUST);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (stlxgfx_font_init(STLXGFX_FONT_PATH) != 0) {
        printf("stlxterm: font init failed\r\n");
    }

    measure_cell();

    int conn = stlxgfx_connect(STLXGFX_DM_SOCKET_PATH);
    if (conn < 0) {
        printf("stlxterm: failed to connect to DM\r\n");
        return 1;
    }

    stlxgfx_window_t* win = stlxgfx_create_window(conn, WIN_WIDTH,
                                                    WIN_HEIGHT, "stlxterm");
    if (!win) {
        printf("stlxterm: failed to create window\r\n");
        stlxgfx_disconnect(conn);
        return 1;
    }

    uint32_t content_w = win->width - 2 * CONTENT_PAD_X;
    uint32_t content_h = win->height - 2 * CONTENT_PAD_Y;
    int term_cols = (int)(content_w / g_cell_w);
    int term_rows = (int)(content_h / g_cell_h);
    if (term_cols < 1) term_cols = 1;
    if (term_rows < 1) term_rows = 1;

    term_state* term = (term_state*)malloc(sizeof(term_state));
    if (!term) {
        printf("stlxterm: failed to allocate term_state\r\n");
        stlxgfx_window_close(win);
        stlxgfx_disconnect(conn);
        return 1;
    }
    term_init(term, term_rows, term_cols, COL_TEXT, COL_BASE);

    printf("stlxterm: grid %dx%d (cell %ux%u, font %d)\r\n",
           term_cols, term_rows, g_cell_w, g_cell_h, FONT_SIZE);

    int master_fd, slave_fd;
    if (pty_create(&master_fd, &slave_fd) < 0) {
        printf("stlxterm: pty_create failed\r\n");
        stlxgfx_window_close(win);
        stlxgfx_disconnect(conn);
        return 1;
    }

    fcntl(master_fd, F_SETFL, O_NONBLOCK);
    ioctl(slave_fd, STLX_TCSETS_RAW, 0);

    int shell_handle = proc_create("/initrd/bin/shell", NULL);
    if (shell_handle < 0) {
        printf("stlxterm: failed to create shell process\r\n");
        close(slave_fd);
        close(master_fd);
        stlxgfx_window_close(win);
        stlxgfx_disconnect(conn);
        return 1;
    }

    proc_set_handle(shell_handle, 0, slave_fd);
    proc_set_handle(shell_handle, 1, slave_fd);
    proc_set_handle(shell_handle, 2, slave_fd);

    if (proc_start(shell_handle) < 0) {
        printf("stlxterm: failed to start shell\r\n");
        close(slave_fd);
        close(master_fd);
        stlxgfx_window_close(win);
        stlxgfx_disconnect(conn);
        return 1;
    }

    close(slave_fd);

    struct timespec frame_interval = { 0, 16000000 };
    int shell_alive = 1;

    while (!stlxgfx_window_should_close(win) && shell_alive) {
        stlxgfx_event_t evt;
        while (stlxgfx_window_next_event(win, &evt) == 1) {
            if (evt.type == STLXGFX_EVT_KEY_DOWN) {
                char seq[8];
                int len = keymap_translate(evt.key.usage, evt.key.modifiers,
                                           seq, sizeof(seq));
                if (len > 0) {
                    write(master_fd, seq, (size_t)len);
                }
            }
        }

        char pty_buf[512];
        ssize_t n = read(master_fd, pty_buf, sizeof(pty_buf));
        if (n > 0) {
            term_feed(term, pty_buf, (int)n);
        } else if (n == 0) {
            shell_alive = 0;
        }

        if (term->dirty) {
            stlxgfx_surface_t* buf = stlxgfx_window_back_buffer(win);
            if (buf) {
                render_term(buf, term);
                stlxgfx_window_swap_buffers(win);
                term->dirty = 0;
            }
        }

        nanosleep(&frame_interval, NULL);
    }

    proc_kill(shell_handle);
    int exit_code = 0;
    proc_wait(shell_handle, &exit_code);

    close(master_fd);
    stlxgfx_window_close(win);
    stlxgfx_disconnect(conn);
    return 0;
}
