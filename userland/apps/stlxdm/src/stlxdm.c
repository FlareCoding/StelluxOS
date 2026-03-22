#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/fb.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/font.h>
#include <stlxgfx/window.h>
#include <stlxgfx/event.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "stlxdm_input.h"

static dm_client_t g_clients[STLXGFX_DM_MAX_CLIENTS];
static int g_client_count;
static stlxdm_input_t g_input;

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

    stlxgfx_surface_t* backbuf = stlxgfx_fb_create_surface(&fb, fb.width, fb.height);
    if (!backbuf) {
        printf("stlxdm: failed to create back buffer\r\n");
        stlxgfx_fb_close(&fb);
        return 1;
    }

    int listen_fd = stlxgfx_dm_listen(STLXGFX_DM_SOCKET_PATH);
    if (listen_fd < 0) {
        printf("stlxdm: failed to bind socket\r\n");
        stlxgfx_destroy_surface(backbuf);
        stlxgfx_fb_close(&fb);
        return 1;
    }
    printf("stlxdm: listening on %s\r\n", STLXGFX_DM_SOCKET_PATH);

    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        g_clients[i].fd = -1;
        g_clients[i].window = NULL;
    }

    stlxdm_input_init(&g_input, (int32_t)fb.width, (int32_t)fb.height);

    struct timespec ts = { 0, 33000000 };

    while (1) {
        if (g_client_count < STLXGFX_DM_MAX_CLIENTS) {
            int client_fd = stlxgfx_dm_accept(listen_fd);
            if (client_fd >= 0) {
                for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
                    if (g_clients[i].fd < 0) {
                        g_clients[i].fd = client_fd;
                        g_clients[i].window = NULL;
                        g_client_count++;
                        printf("stlxdm: client connected (slot %d)\r\n", i);
                        break;
                    }
                }
            }
        }

        for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
            if (g_clients[i].fd < 0) {
                continue;
            }

            stlxgfx_msg_header_t hdr;
            uint8_t payload[512];
            int rc = stlxgfx_dm_read_request(g_clients[i].fd, &hdr, payload, sizeof(payload));
            if (rc < 0) {
                printf("stlxdm: client disconnected (slot %d)\r\n", i);
                if (g_clients[i].window) {
                    stlxdm_input_remove_window(&g_input, i);
                    stlxgfx_dm_destroy_window(g_clients[i].window);
                    g_clients[i].window = NULL;
                }
                close(g_clients[i].fd);
                g_clients[i].fd = -1;
                g_client_count--;
                continue;
            }
            if (rc == 0) {
                continue;
            }

            if (hdr.message_type == STLXGFX_MSG_CREATE_WINDOW_REQ && !g_clients[i].window) {
                stlxgfx_create_window_req_t* req = (stlxgfx_create_window_req_t*)payload;
                stlxgfx_dm_window_t* win = stlxgfx_dm_handle_create_window(
                    g_clients[i].fd, &hdr, req, &fb);
                if (win) {
                    g_clients[i].window = win;
                    stlxdm_input_add_window(&g_input, i);
                    printf("stlxdm: created window %u (%ux%u)\r\n",
                           win->window_id, win->width, win->height);
                }
            } else if (hdr.message_type == STLXGFX_MSG_DESTROY_WINDOW_REQ) {
                if (g_clients[i].window) {
                    stlxdm_input_remove_window(&g_input, i);
                    stlxgfx_dm_destroy_window(g_clients[i].window);
                    g_clients[i].window = NULL;
                }
            }
        }

        stlxdm_input_process(&g_input, g_clients, STLXGFX_DM_MAX_CLIENTS);

        for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
            if (g_clients[i].window) {
                stlxgfx_dm_sync(g_clients[i].window);
            }
        }

        stlxgfx_clear(backbuf, 0xFF2D2D30);
        stlxgfx_draw_text(backbuf, 10, 10, "Stellux Display Manager", 16, 0xFFFFFFFF);

        for (int i = 0; i < g_input.z_count; i++) {
            int slot = stlxdm_input_z_order(&g_input, i);
            if (slot < 0 || !g_clients[slot].window) {
                continue;
            }
            stlxgfx_surface_t* front = stlxgfx_dm_front_buffer(g_clients[slot].window);
            if (front) {
                stlxgfx_blit(backbuf, g_clients[slot].window->x,
                             g_clients[slot].window->y,
                             front, 0, 0, front->width, front->height);
            }
        }

        stlxdm_input_draw_cursor(&g_input, backbuf);

        stlxgfx_fb_present(&fb, backbuf);

        for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
            if (g_clients[i].window) {
                stlxgfx_dm_finish_sync(g_clients[i].window);
            }
        }

        nanosleep(&ts, NULL);
    }
}
