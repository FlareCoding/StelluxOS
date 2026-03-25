#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/window.h>
#include <stlxgfx/font.h>
#include <stlx/proc.h>
#include <stlx/pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>

#include "keymap.h"
#include "term.h"

#define STLX_TCSETS_RAW       0x5401

#define STLXTERM_WIDTH        800
#define STLXTERM_HEIGHT       600
#define STLXTERM_BG_COLOR     0xFF1E1E2E
#define STLXTERM_FG_COLOR     0xFFCDD6F4
#define STLXTERM_CURSOR_COLOR 0xFFF5E0DC
#define STLXTERM_FONT_SIZE    16
#define STLXTERM_PADDING      4
#define STLXTERM_FRAME_NS     33000000
#define STLXTERM_BLINK_FRAMES 15

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

static void render_term(stlxgfx_surface_t *buf, const term_state_t *t,
                        uint32_t cell_w, uint32_t cell_h, int cursor_visible) {
    stlxgfx_clear(buf, STLXTERM_BG_COLOR);

    int32_t pad = STLXTERM_PADDING;
    char ch_buf[2] = {0, 0};
    for (int r = 0; r < t->rows; r++) {
        for (int c = 0; c < t->cols; c++) {
            int32_t px = pad + (int32_t)(c * cell_w);
            int32_t py = pad + (int32_t)(r * cell_h);
            uint8_t bg_idx = t->attrs[r][c].bg;
            if (bg_idx != 0) {
                stlxgfx_fill_rect(buf, px, py,
                    cell_w, cell_h, BG_PALETTE[bg_idx]);
            }
            if (t->cells[r][c] != ' ') {
                uint8_t fg_idx = t->attrs[r][c].fg;
                ch_buf[0] = t->cells[r][c];
                stlxgfx_draw_text(buf, px, py,
                    ch_buf, STLXTERM_FONT_SIZE, FG_PALETTE[fg_idx]);
            }
        }
    }

    if (cursor_visible) {
        int cc = t->cursor_col < t->cols ? t->cursor_col : t->cols - 1;
        int cr = t->cursor_row;
        int32_t cx = pad + (int32_t)(cc * cell_w);
        int32_t cy = pad + (int32_t)(cr * cell_h);

        stlxgfx_fill_rect(buf, cx, cy,
            cell_w, cell_h, STLXTERM_CURSOR_COLOR);

        if (t->cells[cr][cc] != ' ') {
            ch_buf[0] = t->cells[cr][cc];
            stlxgfx_draw_text(buf, cx, cy,
                ch_buf, STLXTERM_FONT_SIZE, STLXTERM_BG_COLOR);
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (stlxgfx_font_init(STLXGFX_FONT_PATH) != 0) {
        printf("stlxterm: failed to load font\r\n");
        return 1;
    }

    int conn = stlxgfx_connect(STLXGFX_DM_SOCKET_PATH);
    if (conn < 0) {
        printf("stlxterm: failed to connect to DM\r\n");
        stlxgfx_font_cleanup();
        return 1;
    }

    stlxgfx_window_t *win = stlxgfx_create_window(conn,
        STLXTERM_WIDTH, STLXTERM_HEIGHT, "stlxterm");
    if (!win) {
        printf("stlxterm: failed to create window\r\n");
        stlxgfx_disconnect(conn);
        stlxgfx_font_cleanup();
        return 1;
    }

    uint32_t cell_w, cell_h;
    stlxgfx_text_size("M", STLXTERM_FONT_SIZE, &cell_w, &cell_h);
    if (cell_w == 0) cell_w = 8;
    if (cell_h == 0) cell_h = 16;
    int term_cols = (int)((STLXTERM_WIDTH  - 2 * STLXTERM_PADDING) / cell_w);
    int term_rows = (int)((STLXTERM_HEIGHT - 2 * STLXTERM_PADDING) / cell_h);

    term_state_t *term = malloc(sizeof(term_state_t));
    if (!term) {
        printf("stlxterm: failed to allocate terminal state\r\n");
        stlxgfx_window_close(win);
        stlxgfx_disconnect(conn);
        stlxgfx_font_cleanup();
        return 1;
    }
    term_init(term, term_rows, term_cols);

    int master_fd, slave_fd;
    if (pty_create(&master_fd, &slave_fd) < 0) {
        printf("stlxterm: failed to create PTY\r\n");
        free(term);
        stlxgfx_window_close(win);
        stlxgfx_disconnect(conn);
        stlxgfx_font_cleanup();
        return 1;
    }

    ioctl(slave_fd, STLX_TCSETS_RAW, 0);
    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    int shell_proc = proc_create("/initrd/bin/shell", NULL);
    if (shell_proc < 0) {
        printf("stlxterm: failed to create shell\r\n");
        close(master_fd);
        close(slave_fd);
        free(term);
        stlxgfx_window_close(win);
        stlxgfx_disconnect(conn);
        stlxgfx_font_cleanup();
        return 1;
    }
    proc_set_handle(shell_proc, 0, slave_fd);
    proc_set_handle(shell_proc, 1, slave_fd);
    proc_set_handle(shell_proc, 2, slave_fd);
    if (proc_start(shell_proc) < 0) {
        printf("stlxterm: failed to start shell\r\n");
        close(shell_proc);
        close(master_fd);
        close(slave_fd);
        free(term);
        stlxgfx_window_close(win);
        stlxgfx_disconnect(conn);
        stlxgfx_font_cleanup();
        return 1;
    }
    close(slave_fd);

    struct timespec frame_interval = { 0, STLXTERM_FRAME_NS };
    int running = 1;
    int blink_counter = 0;
    int cursor_visible = 1;

    while (running && !stlxgfx_window_should_close(win)) {
        stlxgfx_event_t evt;
        while (stlxgfx_window_next_event(win, &evt) == 1) {
            if (evt.type == STLXGFX_EVT_KEY_DOWN) {
                char seq[8];
                int len = keymap_translate(evt.key.usage, evt.key.modifiers,
                                           seq, sizeof(seq));
                if (len > 0) {
                    write(master_fd, seq, (size_t)len);
                    cursor_visible = 1;
                    blink_counter = 0;
                    term->dirty = 1;
                }
            }
        }

        char pty_buf[512];
        ssize_t n = read(master_fd, pty_buf, sizeof(pty_buf));
        if (n == 0) {
            running = 0;
            continue;
        }
        if (n > 0) {
            term_feed(term, pty_buf, (int)n);
            cursor_visible = 1;
            blink_counter = 0;
        }

        blink_counter++;
        if (blink_counter >= STLXTERM_BLINK_FRAMES) {
            blink_counter = 0;
            cursor_visible = !cursor_visible;
            term->dirty = 1;
        }

        if (term->dirty) {
            stlxgfx_surface_t *buf = stlxgfx_window_back_buffer(win);
            if (buf) {
                render_term(buf, term, cell_w, cell_h, cursor_visible);
                stlxgfx_window_swap_buffers(win);
                term->dirty = 0;
            }
        }

        nanosleep(&frame_interval, NULL);
    }

    proc_detach(shell_proc);
    close(master_fd);
    free(term);
    stlxgfx_window_close(win);
    stlxgfx_disconnect(conn);
    stlxgfx_font_cleanup();
    return 0;
}
