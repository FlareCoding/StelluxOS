#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/window.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>
#include <stlxgfx/event.h>
#include <stlx/pty.h>
#include <stlx/proc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "keymap.h"
#include "term.h"

#define STLX_TCSETS_RAW 0x5401

#define TERM_BG_COLOR    0xFF1E1E2E
#define TERM_FG_COLOR    0xFFCDD6F4
#define TERM_CURSOR_COLOR 0xFFF5E0DC
#define TERM_FONT_SIZE   16
#define TERM_WIN_WIDTH   720
#define TERM_WIN_HEIGHT  480

static uint32_t g_cell_w;
static uint32_t g_cell_h;

static void measure_cell(void) {
    stlxgfx_text_size("M", TERM_FONT_SIZE, &g_cell_w, &g_cell_h);
    if (g_cell_w == 0) g_cell_w = 10;
    if (g_cell_h == 0) g_cell_h = 18;
}

static void render_term(stlxgfx_surface_t* buf, term_state* t) {
    stlxgfx_clear(buf, TERM_BG_COLOR);

    char ch_str[2] = {0, 0};
    for (int r = 0; r < t->rows; r++) {
        for (int c = 0; c < t->cols; c++) {
            char ch = t->cells[r][c];
            if (ch <= ' ') continue;
            ch_str[0] = ch;
            int32_t px = (int32_t)(c * g_cell_w);
            int32_t py = (int32_t)(r * g_cell_h);
            stlxgfx_draw_text(buf, px, py, ch_str, TERM_FONT_SIZE,
                              TERM_FG_COLOR);
        }
    }

    /* Draw block cursor */
    int32_t cx = (int32_t)(t->cursor_col * g_cell_w);
    int32_t cy = (int32_t)(t->cursor_row * g_cell_h);
    stlxgfx_fill_rect(buf, cx, cy, g_cell_w, g_cell_h, TERM_CURSOR_COLOR);

    /* Re-draw the character under the cursor in dark color */
    char under = t->cells[t->cursor_row][t->cursor_col];
    if (under > ' ') {
        ch_str[0] = under;
        stlxgfx_draw_text(buf, cx, cy, ch_str, TERM_FONT_SIZE, TERM_BG_COLOR);
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

    stlxgfx_window_t* win = stlxgfx_create_window(conn, TERM_WIN_WIDTH,
                                                    TERM_WIN_HEIGHT, "stlxterm");
    if (!win) {
        printf("stlxterm: failed to create window\r\n");
        stlxgfx_disconnect(conn);
        return 1;
    }

    int term_cols = (int)(win->width / g_cell_w);
    int term_rows = (int)(win->height / g_cell_h);
    if (term_cols < 1) term_cols = 1;
    if (term_rows < 1) term_rows = 1;

    term_state term;
    term_init(&term, term_rows, term_cols);

    printf("stlxterm: grid %dx%d (cell %ux%u)\r\n",
           term_cols, term_rows, g_cell_w, g_cell_h);

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
        /* 1. Drain window events and forward keys to PTY */
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

        /* 2. Read PTY master output (non-blocking) */
        char pty_buf[512];
        ssize_t n = read(master_fd, pty_buf, sizeof(pty_buf));
        if (n > 0) {
            term_feed(&term, pty_buf, (int)n);
        } else if (n == 0) {
            shell_alive = 0;
        }

        /* 3. Render if dirty */
        if (term.dirty) {
            stlxgfx_surface_t* buf = stlxgfx_window_back_buffer(win);
            if (buf) {
                render_term(buf, &term);
                stlxgfx_window_swap_buffers(win);
                term.dirty = 0;
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
