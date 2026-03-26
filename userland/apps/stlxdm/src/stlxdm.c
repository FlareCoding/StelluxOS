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

#include "stlxdm_conf.h"
#include "stlxdm_decor.h"
#include "stlxdm_input.h"
#include "stlxdm_splash.h"
#include "stlxdm_taskbar.h"

#define STLXDM_FRAME_INTERVAL_NS    16666667
#define STLXDM_BG_COLOR             0xFF2D2D30
#define STLXDM_TITLE_FONT_SIZE       13

/* ---- Dirty region tracking ---- */
#define STLXDM_MAX_DIRTY_RECTS       32

typedef struct {
    int32_t x, y;
    uint32_t w, h;
} stlxdm_rect_t;

typedef struct {
    stlxdm_rect_t rects[STLXDM_MAX_DIRTY_RECTS];
    int count;
    int full_redraw;
} stlxdm_dirty_t;

static void stlxdm_dirty_reset(stlxdm_dirty_t* d) {
    d->count = 0;
    d->full_redraw = 0;
}

static void stlxdm_dirty_add_full(stlxdm_dirty_t* d) {
    d->full_redraw = 1;
}

static void stlxdm_dirty_add_rect(stlxdm_dirty_t* d,
                                    int32_t x, int32_t y,
                                    uint32_t w, uint32_t h) {
    if (d->full_redraw) return;
    if (w == 0 || h == 0) return;
    if (d->count >= STLXDM_MAX_DIRTY_RECTS) {
        d->full_redraw = 1;
        return;
    }
    stlxdm_rect_t* r = &d->rects[d->count++];
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
}

/* Convenience: mark cursor area (sprite + shadow) as dirty */
static void stlxdm_dirty_add_cursor(stlxdm_dirty_t* d,
                                      int32_t px, int32_t py) {
    /* Cursor is 18 wide x 16 tall, shadow is offset +1,+1 */
    stlxdm_dirty_add_rect(d, px, py, 18 + 1, 16 + 1);
}

/* Mark a window's outer frame (including decorations) as dirty */
static void stlxdm_dirty_add_window(stlxdm_dirty_t* d,
                                      stlxgfx_dm_window_t* w) {
    int32_t bw = STLXDM_BORDER_WIDTH;
    /* Include 1px dragging glow border on each side */
    stlxdm_dirty_add_rect(d, w->x - 1, w->y - 1,
                            w->width + 2 * (uint32_t)bw + 2,
                            w->height + STLXDM_TITLE_HEIGHT + (uint32_t)bw + 2);
}

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
            if (srv->clients[i].window) {
                stlxdm_input_remove_window(inp, i, srv->clients);
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
                stlxdm_input_add_window(inp, i, srv->clients);
            }
        } else if (hdr.message_type == STLXGFX_MSG_DESTROY_WINDOW_REQ) {
            if (srv->clients[i].window) {
                stlxdm_input_remove_window(inp, i, srv->clients);
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
    if (!comp->backbuf) {
        return -1;
    }
    return 0;
}

static void stlxdm_compositor_sync(stlxdm_compositor_t* comp,
                                    dm_client_t* clients,
                                    stlxdm_dirty_t* dirty) {
    (void)comp;
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        if (clients[i].window) {
            int had_new = stlxgfx_dm_sync(clients[i].window);
            if (had_new) {
                stlxdm_dirty_add_window(dirty, clients[i].window);
            }
        }
    }
}

static void stlxdm_compositor_draw_bar(stlxgfx_ctx_t* ctx, uint32_t width,
                                        const stlxdm_config_t* conf) {
    stlxgfx_ctx_fill_rect(ctx, 0, 0, width, STLXDM_BAR_HEIGHT,
                           conf->bar_color);
    stlxgfx_ctx_fill_rect(ctx, 0, STLXDM_BAR_HEIGHT, width, 1,
                           conf->accent_color);
    stlxgfx_ctx_draw_text(ctx, 10, 6, "Stellux", conf->bar_font_size,
                           conf->text_color);

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        struct tm* t = gmtime(&ts.tv_sec);
        if (t) {
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%a %b %e  %H:%M:%S", t);
            uint32_t tw = 0, th = 0;
            stlxgfx_ctx_text_size(time_str, conf->bar_font_size, &tw, &th);
            int32_t tx = ((int32_t)width - (int32_t)tw) / 2;
            stlxgfx_ctx_draw_text(ctx, tx, 6, time_str, conf->bar_font_size,
                                   conf->text_color);
        }
    }
}

static void stlxdm_build_arc_lut(int32_t r, int32_t* lut) {
    for (int32_t i = 0; i <= r; i++) lut[i] = 0;
    int32_t px = 0, py = r, d = 1 - r;
    while (px <= py) {
        if (py <= r && px > lut[py]) {
            lut[py] = px;
        }
        if (px <= r && py > lut[px]) {
            lut[px] = py;
        }
        px++;
        if (d < 0) { d += 2 * px + 1; }
        else { py--; d += 2 * (px - py) + 1; }
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

    /* Title bar: rounded top corners, flat bottom */
    stlxgfx_ctx_fill_rounded_rect(ctx, ox + (int32_t)bw, oy + (int32_t)bw,
                                   outer_w - 2 * bw,
                                   STLXDM_TITLE_HEIGHT - bw,
                                   inner_cr, title_bg);
    stlxgfx_ctx_fill_rect(ctx, ox + (int32_t)bw,
                           oy + STLXDM_TITLE_HEIGHT - (int32_t)inner_cr,
                           outer_w - 2 * bw, inner_cr, title_bg);

    /* Content background: flat top, rounded bottom corners */
    uint32_t content_inner_w = outer_w - 2 * bw;
    uint32_t content_inner_h = outer_h - STLXDM_TITLE_HEIGHT - bw;
    int32_t ci_x = ox + (int32_t)bw;
    int32_t ci_y = oy + STLXDM_TITLE_HEIGHT;

    if (inner_cr > 0 && content_inner_h > inner_cr) {
        stlxgfx_ctx_fill_rect(ctx, ci_x, ci_y,
                               content_inner_w,
                               content_inner_h - inner_cr,
                               STLXDM_BG_COLOR);

        int32_t ir = (int32_t)inner_cr;
        int32_t arc_lut[32];
        if (ir > 31) {
            ir = 31;
        }
        stlxdm_build_arc_lut(ir, arc_lut);

        int32_t bl_cx = ci_x + ir;
        int32_t br_cx = ci_x + (int32_t)content_inner_w - ir - 1;
        int32_t bot_cy = ci_y + (int32_t)content_inner_h - ir - 1;

        for (int32_t dy = 0; dy <= ir; dy++) {
            int32_t extent = arc_lut[dy];
            int32_t sy = bot_cy + dy;
            stlxgfx_ctx_fill_rect(ctx, bl_cx - extent, sy,
                                   (uint32_t)(extent + 1 + (br_cx - bl_cx) + extent),
                                   1, STLXDM_BG_COLOR);
        }
    } else {
        stlxgfx_ctx_fill_rect(ctx, ci_x, ci_y,
                               content_inner_w, content_inner_h,
                               STLXDM_BG_COLOR);
    }

    /* Title/content separator */
    stlxgfx_ctx_fill_rect(ctx, ox + (int32_t)bw,
                           oy + STLXDM_TITLE_HEIGHT - 1,
                           outer_w - 2 * bw, 1, border_color);

    /* Title text */
    uint32_t tw = 0, th = 0;
    stlxgfx_ctx_text_size(w->title, STLXDM_TITLE_FONT_SIZE, &tw, &th);
    int32_t title_y = oy + (int32_t)(STLXDM_TITLE_HEIGHT - th) / 2;
    uint32_t text_color = focused ? STLXDM_TITLE_TEXT_FOCUSED
                                  : STLXDM_TITLE_TEXT_UNFOCUSED;
    stlxgfx_ctx_draw_text(ctx, ox + 12, title_y,
                           w->title, STLXDM_TITLE_FONT_SIZE, text_color);

    /* Close button */
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
                                       dm_client_t* clients,
                                       const stlxdm_config_t* conf,
                                       stlxdm_taskbar_t* taskbar) {
    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, comp->backbuf);

    stlxgfx_ctx_clear(&ctx, conf->bg_color);
    stlxdm_compositor_draw_bar(&ctx, comp->width, conf);

    for (int i = 0; i < inp->z_count; i++) {
        int slot = stlxdm_input_z_order(inp, i);
        if (slot < 0 || !clients[slot].window) {
            continue;
        }

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
            int32_t ro = (int32_t)STLXDM_CORNER_RADIUS;
            int32_t ri = (int32_t)(STLXDM_CORNER_RADIUS > bw ? STLXDM_CORNER_RADIUS - bw : 0);
            if (ro > 31) {
                ro = 31;
            }
            if (ri > 31) {
                ri = 31;
            }

            int32_t outer_lut[32], inner_lut[32];
            stlxdm_build_arc_lut(ro, outer_lut);
            if (ri > 0) {
                stlxdm_build_arc_lut(ri, inner_lut);
            }

            int32_t bot_edge = w->y + (int32_t)outer_h;
            int32_t right_edge = w->x + (int32_t)outer_w;

            int32_t outer_cy = bot_edge - ro - 1;
            int32_t inner_cy = bot_edge - (int32_t)bw - ri - 1;

            uint32_t border_c = dragging ? STLXDM_BORDER_DRAGGING
                              : focused  ? STLXDM_BORDER_FOCUSED
                              :            STLXDM_BORDER_UNFOCUSED;

            for (int32_t sy = outer_cy; sy < bot_edge; sy++) {
                int32_t ody = sy - outer_cy;
                if (ody < 0 || ody > ro) {
                    continue;
                }
                int32_t outer_ext = outer_lut[ody];

                int32_t left_bg = ro - outer_ext;
                if (left_bg > 0) {
                    stlxgfx_ctx_fill_rect(&ctx, w->x, sy,
                                           (uint32_t)left_bg, 1,
                                           conf->bg_color);
                    stlxgfx_ctx_fill_rect(&ctx, right_edge - left_bg, sy,
                                           (uint32_t)left_bg, 1,
                                           conf->bg_color);
                }

                if (ri > 0) {
                    int32_t idy = sy - inner_cy;
                    int32_t inner_ext = 0;
                    if (idy >= 0 && idy <= ri) {
                        inner_ext = inner_lut[idy];
                    } else if (idy < 0) {
                        inner_ext = ri;
                    }

                    int32_t inner_left = (int32_t)bw + ri - inner_ext;
                    int32_t outer_left = ro - outer_ext;
                    if (inner_left > outer_left) {
                        int32_t fill_w = inner_left - outer_left;
                        stlxgfx_ctx_fill_rect(&ctx,
                            w->x + outer_left, sy,
                            (uint32_t)fill_w, 1, border_c);
                        stlxgfx_ctx_fill_rect(&ctx,
                            right_edge - inner_left, sy,
                            (uint32_t)fill_w, 1, border_c);
                    }
                }
            }
        }
    }

    stlxdm_taskbar_draw(taskbar, &ctx);
    stlxdm_input_draw_cursor(inp, comp->backbuf);
}

static void stlxdm_compositor_present(stlxdm_compositor_t* comp,
                                       const stlxdm_dirty_t* dirty) {
    if (dirty->full_redraw || dirty->count == 0) {
        /* Full present: either explicitly requested or nothing dirty
         * (first frame / fallback). We still present on count==0 for
         * the first frame and the always-updating clock bar. */
        stlxgfx_fb_present(comp->fb, comp->backbuf);
    } else {
        for (int i = 0; i < dirty->count; i++) {
            const stlxdm_rect_t* r = &dirty->rects[i];
            stlxgfx_fb_present_region(comp->fb, comp->backbuf,
                                       r->x, r->y, r->w, r->h);
        }
    }
}

static void stlxdm_compositor_finish_sync(stlxdm_compositor_t* comp,
                                            dm_client_t* clients) {
    (void)comp;
    for (int i = 0; i < STLXGFX_DM_MAX_CLIENTS; i++) {
        if (clients[i].window)
            stlxgfx_dm_finish_sync(clients[i].window);
    }
}

static void stlxdm_spawn_app(const char* path) {
    int handle = proc_exec(path, NULL);
    if (handle >= 0) {
        proc_detach(handle);
        printf("stlxdm: spawned %s\r\n", path);
    } else {
        printf("stlxdm: failed to spawn %s\r\n", path);
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

    stlxdm_config_t config;
    if (stlxdm_conf_load(&config, STLXDM_CONF_PATH) != 0) {
        printf("stlxdm: no config file, using defaults\r\n");
        stlxdm_conf_defaults(&config);
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

    if (config.autostart_count > 0) {
        for (int i = 0; i < config.autostart_count; i++)
            stlxdm_spawn_app(config.autostart[i].path);
    } else {
        stlxdm_spawn_app("/initrd/bin/stlxterm");
    }

    stlxdm_server_t server;
    stlxdm_server_init(&server, listen_fd);

    stlxdm_input_t input;
    stlxdm_input_init(&input, (int32_t)fb.width, (int32_t)fb.height);
    input.taskbar_height = (int32_t)config.taskbar_height;

    stlxdm_taskbar_t taskbar;
    stlxdm_taskbar_init(&taskbar, &config, fb.width, fb.height);

    stlxdm_dirty_t dirty;
    int prev_focused = -1;
    int prev_close_hover = -1;
    int prev_drag = -1;
    int prev_taskbar_hover = -1;
    int first_frame = 1;

    while (1) {
        struct timespec frame_start;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        stlxdm_dirty_reset(&dirty);

        /* First frame always does a full redraw */
        if (first_frame) {
            stlxdm_dirty_add_full(&dirty);
            first_frame = 0;
        }

        /* Record pre-frame cursor position for dirty tracking */
        int32_t old_ptr_x = input.ptr_x;
        int32_t old_ptr_y = input.ptr_y;
        int old_z_count = input.z_count;

        stlxdm_server_accept(&server);
        stlxdm_server_process_messages(&server, &input, &fb);
        stlxdm_input_process(&input, server.clients, STLXGFX_DM_MAX_CLIENTS,
                              &taskbar);

        if (input.spawn_terminal_requested) {
            stlxdm_spawn_app("/initrd/bin/stlxterm");
        }

        if (taskbar.launch_path[0] != '\0') {
            stlxdm_spawn_app(taskbar.launch_path);
            taskbar.launch_path[0] = '\0';
        }

        /* Detect structural changes that need full redraw */
        if (input.z_count != old_z_count) {
            stlxdm_dirty_add_full(&dirty);
        }
        if (input.focused_slot != prev_focused) {
            stlxdm_dirty_add_full(&dirty);
            prev_focused = input.focused_slot;
        }

        /* Cursor moved → dirty old and new positions */
        if (input.ptr_x != old_ptr_x || input.ptr_y != old_ptr_y) {
            stlxdm_dirty_add_cursor(&dirty, old_ptr_x, old_ptr_y);
            stlxdm_dirty_add_cursor(&dirty, input.ptr_x, input.ptr_y);
        }

        /* Window dragging → dirty the dragged window area */
        if (input.drag_slot >= 0) {
            stlxdm_dirty_add_full(&dirty);
        }
        if (prev_drag >= 0 && input.drag_slot < 0) {
            stlxdm_dirty_add_full(&dirty);
        }
        prev_drag = input.drag_slot;

        /* Close button hover changed → dirty window title area */
        if (input.close_hover_slot != prev_close_hover) {
            if (prev_close_hover >= 0 && server.clients[prev_close_hover].window) {
                stlxdm_dirty_add_window(&dirty,
                                         server.clients[prev_close_hover].window);
            }
            if (input.close_hover_slot >= 0 &&
                server.clients[input.close_hover_slot].window) {
                stlxdm_dirty_add_window(&dirty,
                                         server.clients[input.close_hover_slot].window);
            }
            prev_close_hover = input.close_hover_slot;
        }

        /* Taskbar hover changed */
        if (taskbar.hover_index != prev_taskbar_hover) {
            stlxdm_dirty_add_rect(&dirty, 0, taskbar.bar_y,
                                   fb.width, config.taskbar_height);
            prev_taskbar_hover = taskbar.hover_index;
        }

        /* Always dirty the top status bar (clock updates) */
        stlxdm_dirty_add_rect(&dirty, 0, 0, fb.width,
                               STLXDM_BAR_HEIGHT + 1);

        /* Sync client buffers and mark windows with new frames as dirty */
        stlxdm_compositor_sync(&compositor, server.clients, &dirty);

        stlxdm_compositor_compose(&compositor, &input, server.clients,
                                   &config, &taskbar);
        stlxdm_compositor_present(&compositor, &dirty);
        stlxdm_compositor_finish_sync(&compositor, server.clients);

        struct timespec frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        uint64_t elapsed_ns = (uint64_t)(frame_end.tv_sec - frame_start.tv_sec)
                                  * 1000000000ULL
                            + (uint64_t)(frame_end.tv_nsec - frame_start.tv_nsec);
        if (elapsed_ns < STLXDM_FRAME_INTERVAL_NS) {
            struct timespec rem = {
                0, (long)(STLXDM_FRAME_INTERVAL_NS - elapsed_ns)
            };
            nanosleep(&rem, NULL);
        }
    }
}
