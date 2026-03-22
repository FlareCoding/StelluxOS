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
#include <errno.h>
#include <sys/ioctl.h>

#define STLX_TCSETS_RAW 0x5401

#define TERM_BG_COLOR    0xFF1E1E2E
#define TERM_FG_COLOR    0xFFCDD6F4
#define TERM_FONT_SIZE   16
#define TERM_WIN_WIDTH   720
#define TERM_WIN_HEIGHT  480

#define TERM_CONNECT_RETRIES    20
#define TERM_CONNECT_DELAY_MS   200

static int connect_with_retry(void) {
    struct timespec delay = { 0, TERM_CONNECT_DELAY_MS * 1000000L };
    for (int i = 0; i < TERM_CONNECT_RETRIES; i++) {
        int conn = stlxgfx_connect(STLXGFX_DM_SOCKET_PATH);
        if (conn >= 0) return conn;
        nanosleep(&delay, NULL);
    }
    return -1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (stlxgfx_font_init(STLXGFX_FONT_PATH) != 0) {
        printf("stlxterm: font init failed\r\n");
    }

    int conn = connect_with_retry();
    if (conn < 0) {
        printf("stlxterm: failed to connect to DM\r\n");
        return 1;
    }
    printf("stlxterm: connected to DM\r\n");

    stlxgfx_window_t* win = stlxgfx_create_window(conn, TERM_WIN_WIDTH,
                                                    TERM_WIN_HEIGHT, "stlxterm");
    if (!win) {
        printf("stlxterm: failed to create window\r\n");
        stlxgfx_disconnect(conn);
        return 1;
    }
    printf("stlxterm: window created (%ux%u)\r\n", win->width, win->height);

    int master_fd, slave_fd;
    if (pty_create(&master_fd, &slave_fd) < 0) {
        printf("stlxterm: pty_create failed\r\n");
        stlxgfx_window_close(win);
        stlxgfx_disconnect(conn);
        return 1;
    }
    printf("stlxterm: PTY created (master=%d, slave=%d)\r\n", master_fd, slave_fd);

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
    printf("stlxterm: shell started\r\n");

    close(slave_fd);

    struct timespec frame_interval = { 0, 16000000 }; /* ~60 FPS */

    while (!stlxgfx_window_should_close(win)) {
        stlxgfx_event_t evt;
        while (stlxgfx_window_next_event(win, &evt) == 1) {
            /* Phase 2: keyboard handling will go here */
            (void)evt;
        }

        char pty_buf[512];
        ssize_t n = read(master_fd, pty_buf, sizeof(pty_buf));
        if (n == 0) {
            break; /* shell exited, PTY EOF */
        }
        /* Phase 3: feed pty_buf into terminal emulator */
        (void)n;

        stlxgfx_surface_t* buf = stlxgfx_window_back_buffer(win);
        if (!buf) {
            nanosleep(&frame_interval, NULL);
            continue;
        }

        stlxgfx_clear(buf, TERM_BG_COLOR);
        stlxgfx_draw_text(buf, 16, 16, "stlxterm", TERM_FONT_SIZE, TERM_FG_COLOR);
        stlxgfx_draw_text(buf, 16, 40, "Shell connected via PTY",
                          TERM_FONT_SIZE, 0xFF89B4FA);
        stlxgfx_window_swap_buffers(win);

        nanosleep(&frame_interval, NULL);
    }

    proc_kill(shell_handle);
    int exit_code = 0;
    proc_wait(shell_handle, &exit_code);
    printf("stlxterm: shell exited with code %d\r\n", exit_code);

    close(master_fd);
    stlxgfx_window_close(win);
    stlxgfx_disconnect(conn);
    printf("stlxterm: closed\r\n");
    return 0;
}
