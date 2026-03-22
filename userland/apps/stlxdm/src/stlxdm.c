#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/fb.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>
#include <stlxgfx/window.h>
#include <stlxgfx/event.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stlxdm_input.h"
#include "stlxdm_splash.h"

/* --- Constants --- */

#define STLXDM_FRAME_INTERVAL_NS   33000000
#define STLXDM_BG_COLOR            0xFF2D2D30
#define STLXDM_BAR_COLOR           0xFF1E1E1E
#define STLXDM_BAR_HEIGHT          28
#define STLXDM_BAR_FONT_SIZE       14
#define STLXDM_BAR_TEXT_COLOR      0xFFCCCCCC
#define STLXDM_BAR_ACCENT_COLOR   0xFF888888

/* --- Server layer --- */

typedef struct {
    int            listen_fd;
    dm_client_t    clients[STLXGFX_DM_MAX_CLIENTS];
    int            client_count;
} stlxdm_server_t;

static void stlxdm_server_init(stlxdm_server_t* srv, int listen_fd) {
    srv->listen_fd = listen_fd;
    srv->client_count = 0;
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        srv->clients[i].fd = -1;
        srv->clients[i].window = NULL;
    }
}

static void stlxdm_server_accept(stlxdm_server_t* srv) {
    if (srv->client_count >= STLXGFX_DM_MAX_CLIENTS) {
        return;
    }
    int client_fd = stlxgfx_dm_accept(srv->listen_fd);
    if (client_fd < 0) {
        return;
    }
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        if (srv->clients[i].fd < 0) {
            srv->clients[i].fd = client_fd;
            srv->clients[i].window = NULL;
            srv->client_count++;
            printf("stlxdm: client connected (slot %d)\r\n", i);
            return;
        }
    }
    close(client_fd);
}

static void stlxdm_server_process_messages(stlxdm_server_t* srv,
                                            stlxdm_input_t* inp,
                                            const stlxgfx_fb_t* fb) {
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        if (srv->clients[i].fd < 0) {
            continue;
        }

        stlxgfx_msg_header_t hdr;
        uint8_t payload[512];
        int rc = stlxgfx_dm_read_request(srv->clients[i].fd, &hdr,
                                          payload, sizeof(payload));
        if (rc < 0) {
            printf("stlxdm: client disconnected (slot %d)\r\n", i);
            if (srv->clients[i].window) {
                stlxdm_input_remove_window(inp, i);
                stlxgfx_dm_destroy_window(srv->clients[i].window);
                srv->clients[i].window = NULL;
            }
            close(srv->clients[i].fd);
            srv->clients[i].fd = -1;
            srv->client_count--;
            continue;
        }
        if (rc == 0) {
            continue;
        }

        if (hdr.message_type == STLXGFX_MSG_CREATE_WINDOW_REQ &&
            !srv->clients[i].window) {
            stlxgfx_create_window_req_t* req =
                (stlxgfx_create_window_req_t*)payload;
            stlxgfx_dm_window_t* win = stlxgfx_dm_handle_create_window(
                srv->clients[i].fd, &hdr, req, fb);
            if (win) {
                srv->clients[i].window = win;
                stlxdm_input_add_window(inp, i);
                printf("stlxdm: created window %u (%ux%u)\r\n",
                       win->window_id, win->width, win->height);
            }
        } else if (hdr.message_type == STLXGFX_MSG_DESTROY_WINDOW_REQ) {
            if (srv->clients[i].window) {
                stlxdm_input_remove_window(inp, i);
                stlxgfx_dm_destroy_window(srv->clients[i].window);
                srv->clients[i].window = NULL;
            }
        }
    }
}

/* --- Compositor layer --- */

typedef struct {
    stlxgfx_fb_t*       fb;
    stlxgfx_surface_t*  backbuf;
    uint32_t             width;
    uint32_t             height;
} stlxdm_compositor_t;

static int stlxdm_compositor_init(stlxdm_compositor_t* comp,
                                   stlxgfx_fb_t* fb) {
    comp->fb = fb;
    comp->width = fb->width;
    comp->height = fb->height;
    comp->backbuf = stlxgfx_fb_create_surface(fb, fb->width, fb->height);
    if (!comp->backbuf) {
        return -1;
    }
    return 0;
}

static void stlxdm_compositor_sync(stlxdm_compositor_t* comp,
                                    dm_client_t* clients) {
    (void)comp;
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        if (clients[i].window) {
            stlxgfx_dm_sync(clients[i].window);
        }
    }
}

static void stlxdm_compositor_draw_bar(stlxdm_compositor_t* comp) {
    stlxgfx_fill_rect(comp->backbuf, 0, 0,
                       (int32_t)comp->width, STLXDM_BAR_HEIGHT,
                       STLXDM_BAR_COLOR);

    stlxgfx_fill_rect(comp->backbuf, 0, STLXDM_BAR_HEIGHT,
                       (int32_t)comp->width, 1, STLXDM_BAR_ACCENT_COLOR);

    stlxgfx_draw_text(comp->backbuf, 10, 6,
                       "Stellux", STLXDM_BAR_FONT_SIZE,
                       STLXDM_BAR_TEXT_COLOR);

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        struct tm* t = gmtime(&ts.tv_sec);
        if (t) {
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%a %b %e  %H:%M:%S", t);

            uint32_t tw = 0;
            uint32_t th = 0;
            stlxgfx_text_size(time_str, STLXDM_BAR_FONT_SIZE, &tw, &th);
            int32_t tx = ((int32_t)comp->width - (int32_t)tw) / 2;
            stlxgfx_draw_text(comp->backbuf, tx, 6,
                               time_str, STLXDM_BAR_FONT_SIZE,
                               STLXDM_BAR_TEXT_COLOR);
        }
    }
}

static void stlxdm_compositor_compose(stlxdm_compositor_t* comp,
                                       stlxdm_input_t* inp,
                                       dm_client_t* clients) {
    stlxgfx_clear(comp->backbuf, STLXDM_BG_COLOR);

    stlxdm_compositor_draw_bar(comp);

    for (int i = 0; i < inp->z_count; i++) {
        int slot = stlxdm_input_z_order(inp, i);
        if (slot < 0 || !clients[slot].window) {
            continue;
        }
        stlxgfx_surface_t* front = stlxgfx_dm_front_buffer(clients[slot].window);
        if (front) {
            stlxgfx_blit(comp->backbuf,
                          clients[slot].window->x, clients[slot].window->y,
                          front, 0, 0, front->width, front->height);
        }
    }

    stlxdm_input_draw_cursor(inp, comp->backbuf);
}

static void stlxdm_compositor_present(stlxdm_compositor_t* comp) {
    stlxgfx_fb_present(comp->fb, comp->backbuf);
}

static void stlxdm_compositor_finish_sync(stlxdm_compositor_t* comp,
                                            dm_client_t* clients) {
    (void)comp;
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        if (clients[i].window) {
            stlxgfx_dm_finish_sync(clients[i].window);
        }
    }
}

/* --- Main --- */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    stlxgfx_fb_t fb;
    if (stlxgfx_fb_open(&fb) != 0) {
        printf("stlxdm: failed to open framebuffer\r\n");
        return 1;
    }
    printf("stlxdm: framebuffer %ux%u %ubpp\r\n",
           fb.width, fb.height, (unsigned int)fb.bpp);

    if (stlxgfx_font_init(STLXGFX_FONT_PATH) != 0) {
        printf("stlxdm: font init failed\r\n");
    }

    stlxdm_compositor_t compositor;
    if (stlxdm_compositor_init(&compositor, &fb) != 0) {
        printf("stlxdm: failed to create compositor\r\n");
        stlxgfx_fb_close(&fb);
        return 1;
    }

    stlxdm_show_splash(&fb, compositor.backbuf);

    int listen_fd = stlxgfx_dm_listen(STLXGFX_DM_SOCKET_PATH);
    if (listen_fd < 0) {
        printf("stlxdm: failed to bind socket\r\n");
        stlxgfx_fb_close(&fb);
        return 1;
    }
    printf("stlxdm: listening on %s\r\n", STLXGFX_DM_SOCKET_PATH);

    stlxdm_server_t server;
    stlxdm_server_init(&server, listen_fd);

    stlxdm_input_t input;
    stlxdm_input_init(&input, (int32_t)fb.width, (int32_t)fb.height);

    struct timespec frame_interval = { 0, STLXDM_FRAME_INTERVAL_NS };

    while (1) {
        stlxdm_server_accept(&server);
        stlxdm_server_process_messages(&server, &input, &fb);
        stlxdm_input_process(&input, server.clients, STLXGFX_DM_MAX_CLIENTS);
        stlxdm_compositor_sync(&compositor, server.clients);
        stlxdm_compositor_compose(&compositor, &input, server.clients);
        stlxdm_compositor_present(&compositor);
        stlxdm_compositor_finish_sync(&compositor, server.clients);
        nanosleep(&frame_interval, NULL);
    }
}
