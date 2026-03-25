#define _POSIX_C_SOURCE 199309L
#include <stlxgfx/fb.h>
#include <stlxgfx/surface.h>
#include <stlxgfx/ctx.h>
#include <stlxgfx/font.h>
#include <stlxgfx/window.h>
#include <stlxgfx/event.h>
#include <stlx/proc.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "stlxdm_input.h"
#include "stlxdm_splash.h"

#define STLXDM_FRAME_INTERVAL_NS    16666667
#define STLXDM_BG_COLOR             0xFF2D2D30
#define STLXDM_BAR_COLOR            0xFF1E1E1E
#define STLXDM_BAR_HEIGHT           28
#define STLXDM_BAR_FONT_SIZE        14
#define STLXDM_BAR_TEXT_COLOR        0xFFCCCCCC
#define STLXDM_BAR_ACCENT_COLOR     0xFF888888

#define STLXDM_TITLE_HEIGHT          32
#define STLXDM_BORDER_WIDTH          2
#define STLXDM_CORNER_RADIUS         8
#define STLXDM_CLOSE_BTN_RADIUS      10
#define STLXDM_CLOSE_BTN_MARGIN      8
#define STLXDM_TITLE_FONT_SIZE       13

#define STLXDM_TITLE_BG_FOCUSED      0xFF313244
#define STLXDM_TITLE_BG_UNFOCUSED    0xFF1E1E2E
#define STLXDM_TITLE_TEXT_FOCUSED     0xFFBAC2DE
#define STLXDM_TITLE_TEXT_UNFOCUSED   0xFF585B70
#define STLXDM_BORDER_FOCUSED        0xFF585B70
#define STLXDM_BORDER_DRAGGING       0xFF89B4FA
#define STLXDM_BORDER_UNFOCUSED      0xFF313244
#define STLXDM_CLOSE_NORMAL_BG       0xFF45475A
#define STLXDM_CLOSE_HOVER_BG        0xFFF38BA8
#define STLXDM_CLOSE_PRESS_BG        0xFFD06080
#define STLXDM_CLOSE_X_NORMAL        0xFFBAC2DE
#define STLXDM_CLOSE_X_HOVER         0xFFFFFFFF

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
    if (srv->client_count >= STLXGFX_DM_MAX_CLIENTS) return;
    int client_fd = stlxgfx_dm_accept(srv->listen_fd);
    if (client_fd < 0) return;
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        if (srv->clients[i].fd < 0) {
            srv->clients[i].fd = client_fd;
            srv->clients[i].window = NULL;
            srv->client_count++;
            return;
        }
    }
    close(client_fd);
}

static void stlxdm_server_process_messages(stlxdm_server_t* srv,
                                            stlxdm_input_t* inp,
                                            const stlxgfx_fb_t* fb) {
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        if (srv->clients[i].fd < 0) continue;

        stlxgfx_msg_header_t hdr;
        uint8_t payload[512];
        int rc = stlxgfx_dm_read_request(srv->clients[i].fd, &hdr,
                                          payload, sizeof(payload));
        if (rc < 0) {
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
        if (rc == 0) continue;

        if (hdr.message_type == STLXGFX_MSG_CREATE_WINDOW_REQ &&
            !srv->clients[i].window) {
            stlxgfx_create_window_req_t* req =
                (stlxgfx_create_window_req_t*)payload;
            stlxgfx_dm_window_t* win = stlxgfx_dm_handle_create_window(
                srv->clients[i].fd, &hdr, req, fb);
            if (win) {
                srv->clients[i].window = win;
                stlxdm_input_add_window_with_focus(inp, i, srv->clients);
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
    if (!comp->backbuf) return -1;
    return 0;
}

static void stlxdm_compositor_sync(stlxdm_compositor_t* comp,
                                    dm_client_t* clients) {
    (void)comp;
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        if (clients[i].window)
            stlxgfx_dm_sync(clients[i].window);
    }
}

static void stlxdm_compositor_draw_bar(stlxgfx_ctx_t* ctx, uint32_t width) {
    stlxgfx_ctx_fill_rect(ctx, 0, 0, width, STLXDM_BAR_HEIGHT, STLXDM_BAR_COLOR);
    stlxgfx_ctx_fill_rect(ctx, 0, STLXDM_BAR_HEIGHT, width, 1, STLXDM_BAR_ACCENT_COLOR);
    stlxgfx_ctx_draw_text(ctx, 10, 6, "Stellux", STLXDM_BAR_FONT_SIZE, STLXDM_BAR_TEXT_COLOR);

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        struct tm* t = gmtime(&ts.tv_sec);
        if (t) {
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%a %b %e  %H:%M:%S", t);
            uint32_t tw = 0, th = 0;
            stlxgfx_ctx_text_size(time_str, STLXDM_BAR_FONT_SIZE, &tw, &th);
            int32_t tx = ((int32_t)width - (int32_t)tw) / 2;
            stlxgfx_ctx_draw_text(ctx, tx, 6, time_str, STLXDM_BAR_FONT_SIZE,
                                   STLXDM_BAR_TEXT_COLOR);
        }
    }
}

static void stlxdm_draw_window_frame(stlxgfx_ctx_t* ctx,
                                      stlxgfx_dm_window_t* w,
                                      int focused, int dragging,
                                      int close_hover, int close_pressed) {
    int32_t ox = w->x;
    int32_t oy = w->y;
    uint32_t bw = STLXDM_BORDER_WIDTH;
    uint32_t outer_w = w->width + 2 * bw;
    uint32_t outer_h = w->height + STLXDM_TITLE_HEIGHT + bw;
    uint32_t cr = STLXDM_CORNER_RADIUS;

    uint32_t border_color = dragging ? STLXDM_BORDER_DRAGGING
                          : focused  ? STLXDM_BORDER_FOCUSED
                          :            STLXDM_BORDER_UNFOCUSED;

    uint32_t title_bg = focused ? STLXDM_TITLE_BG_FOCUSED
                                : STLXDM_TITLE_BG_UNFOCUSED;

    if (dragging) {
        stlxgfx_ctx_fill_rounded_rect(ctx, ox - 1, oy - 1,
                                       outer_w + 2, outer_h + 2,
                                       cr + 1, STLXDM_BORDER_DRAGGING);
    }

    stlxgfx_ctx_fill_rounded_rect(ctx, ox, oy, outer_w, outer_h,
                                   cr, border_color);

    uint32_t inner_cr = cr > bw ? cr - bw : 0;
    stlxgfx_ctx_fill_rounded_rect(ctx, ox + (int32_t)bw, oy + (int32_t)bw,
                                   outer_w - 2 * bw,
                                   STLXDM_TITLE_HEIGHT - bw,
                                   inner_cr, title_bg);
    stlxgfx_ctx_fill_rect(ctx, ox + (int32_t)bw,
                           oy + STLXDM_TITLE_HEIGHT - (int32_t)inner_cr,
                           outer_w - 2 * bw, inner_cr, title_bg);

    stlxgfx_ctx_fill_rect(ctx, ox + (int32_t)bw,
                           oy + STLXDM_TITLE_HEIGHT,
                           outer_w - 2 * bw,
                           outer_h - STLXDM_TITLE_HEIGHT - bw,
                           STLXDM_BG_COLOR);

    stlxgfx_ctx_fill_rect(ctx, ox + (int32_t)bw,
                           oy + STLXDM_TITLE_HEIGHT - 1,
                           outer_w - 2 * bw, 1, border_color);

    uint32_t tw = 0, th = 0;
    stlxgfx_ctx_text_size(w->title, STLXDM_TITLE_FONT_SIZE, &tw, &th);
    int32_t title_y = oy + (int32_t)(STLXDM_TITLE_HEIGHT - th) / 2;
    uint32_t text_color = focused ? STLXDM_TITLE_TEXT_FOCUSED
                                  : STLXDM_TITLE_TEXT_UNFOCUSED;
    stlxgfx_ctx_draw_text(ctx, ox + 12, title_y,
                           w->title, STLXDM_TITLE_FONT_SIZE, text_color);

    if (focused) {
        int32_t ccx = ox + (int32_t)outer_w - STLXDM_CLOSE_BTN_MARGIN
                     - STLXDM_CLOSE_BTN_RADIUS - (int32_t)bw;
        int32_t ccy = oy + STLXDM_TITLE_HEIGHT / 2;
        uint32_t cb_bg = close_pressed ? STLXDM_CLOSE_PRESS_BG
                       : close_hover   ? STLXDM_CLOSE_HOVER_BG
                       :                 STLXDM_CLOSE_NORMAL_BG;
        uint32_t cb_fg = (close_hover || close_pressed) ? STLXDM_CLOSE_X_HOVER
                                                        : STLXDM_CLOSE_X_NORMAL;
        stlxgfx_ctx_fill_circle(ctx, ccx, ccy, STLXDM_CLOSE_BTN_RADIUS, cb_bg);

        uint32_t xw = 0, xh = 0;
        stlxgfx_ctx_text_size("x", STLXDM_TITLE_FONT_SIZE, &xw, &xh);
        stlxgfx_ctx_draw_text(ctx, ccx - (int32_t)xw / 2,
                               ccy - (int32_t)xh / 2,
                               "x", STLXDM_TITLE_FONT_SIZE, cb_fg);
    }
}

static void stlxdm_compositor_compose(stlxdm_compositor_t* comp,
                                       stlxdm_input_t* inp,
                                       dm_client_t* clients) {
    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, comp->backbuf);

    stlxgfx_ctx_clear(&ctx, STLXDM_BG_COLOR);
    stlxdm_compositor_draw_bar(&ctx, comp->width);

    for (int i = 0; i < inp->z_count; i++) {
        int slot = stlxdm_input_z_order(inp, i);
        if (slot < 0 || !clients[slot].window) continue;

        stlxgfx_dm_window_t* w = clients[slot].window;
        int focused = (slot == inp->focused_slot);
        int dragging = (slot == inp->drag_slot);
        int close_hover = (focused && inp->close_hover_slot == slot);
        int close_pressed = (inp->close_press_slot == slot);

        stlxdm_draw_window_frame(&ctx, w,
                                  focused, dragging, close_hover, close_pressed);

        stlxgfx_surface_t* front = stlxgfx_dm_front_buffer(w);
        if (front) {
            int32_t content_x = w->x + STLXDM_BORDER_WIDTH;
            int32_t content_y = w->y + STLXDM_TITLE_HEIGHT;

            stlxgfx_ctx_blit(&ctx, content_x, content_y,
                              front, 0, 0, front->width, front->height);

            uint32_t bw = STLXDM_BORDER_WIDTH;
            uint32_t outer_w = w->width + 2 * bw;
            uint32_t outer_h = w->height + STLXDM_TITLE_HEIGHT + bw;
            int32_t r = (int32_t)STLXDM_CORNER_RADIUS;
            if (r > 31) r = 31;

            int32_t right_edge = w->x + (int32_t)outer_w;
            int32_t bot_edge = w->y + (int32_t)outer_h;
            int32_t cx_br = right_edge - r - 1;
            int32_t cy_bl = bot_edge - r - 1;

            int32_t px2 = 0, py2 = r, d2 = 1 - r;
            while (px2 <= py2) {
                /* Bottom-left scanlines */
                stlxgfx_ctx_fill_rect(&ctx, w->x, cy_bl - px2,
                                       (uint32_t)(r - py2), 1, STLXDM_BG_COLOR);
                stlxgfx_ctx_fill_rect(&ctx, w->x, cy_bl + px2,
                                       (uint32_t)(r - py2), 1, STLXDM_BG_COLOR);
                if (px2 != py2) {
                    stlxgfx_ctx_fill_rect(&ctx, w->x, cy_bl - py2,
                                           (uint32_t)(r - px2), 1, STLXDM_BG_COLOR);
                    stlxgfx_ctx_fill_rect(&ctx, w->x, cy_bl + py2,
                                           (uint32_t)(r - px2), 1, STLXDM_BG_COLOR);
                }

                /* Bottom-right scanlines */
                stlxgfx_ctx_fill_rect(&ctx, cx_br + py2 + 1, cy_bl - px2,
                                       (uint32_t)(r - py2), 1, STLXDM_BG_COLOR);
                stlxgfx_ctx_fill_rect(&ctx, cx_br + py2 + 1, cy_bl + px2,
                                       (uint32_t)(r - py2), 1, STLXDM_BG_COLOR);
                if (px2 != py2) {
                    stlxgfx_ctx_fill_rect(&ctx, cx_br + px2 + 1, cy_bl - py2,
                                           (uint32_t)(r - px2), 1, STLXDM_BG_COLOR);
                    stlxgfx_ctx_fill_rect(&ctx, cx_br + px2 + 1, cy_bl + py2,
                                           (uint32_t)(r - px2), 1, STLXDM_BG_COLOR);
                }

                px2++;
                if (d2 < 0) { d2 += 2 * px2 + 1; }
                else { py2--; d2 += 2 * (px2 - py2) + 1; }
            }
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
        if (clients[i].window)
            stlxgfx_dm_finish_sync(clients[i].window);
    }
}

static void stlxdm_spawn_terminal(void) {
    int handle = proc_exec("/initrd/bin/stlxterm", NULL);
    if (handle >= 0) {
        proc_detach(handle);
        printf("stlxdm: spawned new terminal\r\n");
    } else {
        printf("stlxdm: failed to spawn terminal\r\n");
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    stlxgfx_fb_t fb;
    if (stlxgfx_fb_open(&fb) != 0) {
        printf("stlxdm: failed to open framebuffer\r\n");
        return 1;
    }

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

    stlxdm_spawn_terminal();

    stlxdm_server_t server;
    stlxdm_server_init(&server, listen_fd);

    stlxdm_input_t input;
    stlxdm_input_init(&input, (int32_t)fb.width, (int32_t)fb.height);

    struct timespec frame_interval = { 0, STLXDM_FRAME_INTERVAL_NS };

    while (1) {
        stlxdm_server_accept(&server);
        stlxdm_server_process_messages(&server, &input, &fb);
        stlxdm_input_process(&input, server.clients, STLXGFX_DM_MAX_CLIENTS);

        if (input.spawn_terminal_requested) {
            stlxdm_spawn_terminal();
        }

        stlxdm_compositor_sync(&compositor, server.clients);
        stlxdm_compositor_compose(&compositor, &input, server.clients);
        stlxdm_compositor_present(&compositor);
        stlxdm_compositor_finish_sync(&compositor, server.clients);
        nanosleep(&frame_interval, NULL);
    }
}
