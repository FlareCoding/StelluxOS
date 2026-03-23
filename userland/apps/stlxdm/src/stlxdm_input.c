#define _POSIX_C_SOURCE 199309L
#include "stlxdm_input.h"
#include <stlxgfx/surface.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char* g_cursor_shape[16] = {
    "X                 ",
    "XX                ",
    "X.X               ",
    "X..X              ",
    "X...X             ",
    "X....X            ",
    "X.....X           ",
    "X......X          ",
    "X.......X         ",
    "X........X        ",
    "X.........X       ",
    "X..........X      ",
    "X......XXXXXXX    ",
    "X...XX            ",
    "X..X              ",
    "XX                "
};

void stlxdm_input_init(stlxdm_input_t* inp, int32_t fb_w, int32_t fb_h) {
    memset(inp, 0, sizeof(*inp));
    inp->kbd_fd = open("/dev/input/kbd", O_RDONLY | O_NONBLOCK);
    inp->mouse_fd = open("/dev/input/mouse", O_RDONLY | O_NONBLOCK);
    inp->fb_width = fb_w;
    inp->fb_height = fb_h;
    inp->ptr_x = fb_w / 2;
    inp->ptr_y = fb_h / 2;
    inp->focused_slot = -1;
    inp->capture_slot = -1;
    inp->z_count = 0;

    if (inp->kbd_fd < 0) {
        printf("stlxdm: warning: could not open /dev/input/kbd (errno=%d)\r\n", errno);
    }
    if (inp->mouse_fd < 0) {
        printf("stlxdm: warning: could not open /dev/input/mouse (errno=%d)\r\n", errno);
    }
}

void stlxdm_input_add_window(stlxdm_input_t* inp, int slot) {
    for (int i = 0; i < inp->z_count; i++) {
        if (inp->z_order[i] == slot) {
            return;
        }
    }
    if (inp->z_count < STLXGFX_DM_MAX_CLIENTS) {
        inp->z_order[inp->z_count++] = slot;
    }
    if (inp->focused_slot < 0) {
        inp->focused_slot = slot;
    }
}

void stlxdm_input_remove_window(stlxdm_input_t* inp, int slot) {
    int found = -1;
    for (int i = 0; i < inp->z_count; i++) {
        if (inp->z_order[i] == slot) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        for (int i = found; i < inp->z_count - 1; i++) {
            inp->z_order[i] = inp->z_order[i + 1];
        }
        inp->z_count--;
    }
    if (inp->focused_slot == slot) {
        inp->focused_slot = inp->z_count > 0 ? inp->z_order[inp->z_count - 1] : -1;
    }
    if (inp->capture_slot == slot) {
        inp->capture_slot = -1;
        inp->capture_buttons = 0;
    }
}

int stlxdm_input_z_order(const stlxdm_input_t* inp, int idx) {
    if (idx < 0 || idx >= inp->z_count) {
        return -1;
    }
    return inp->z_order[idx];
}

/* Must match stlxdm.c decoration constants */
#define STLXDM_DECOR_TITLEBAR_H 28
#define STLXDM_DECOR_BORDER_W   1

static int hit_test(const stlxdm_input_t* inp, const dm_client_t* clients,
                    int32_t px, int32_t py) {
    for (int i = inp->z_count - 1; i >= 0; i--) {
        int slot = inp->z_order[i];
        stlxgfx_dm_window_t* w = clients[slot].window;
        if (!w) {
            continue;
        }
        int32_t total_w = (int32_t)w->width + 2 * STLXDM_DECOR_BORDER_W;
        int32_t total_h = (int32_t)w->height + STLXDM_DECOR_TITLEBAR_H
                          + 2 * STLXDM_DECOR_BORDER_W;
        if (px >= w->x && px < w->x + total_w &&
            py >= w->y && py < w->y + total_h) {
            return slot;
        }
    }
    return -1;
}

static void raise_slot(stlxdm_input_t* inp, int slot) {
    int found = -1;
    for (int i = 0; i < inp->z_count; i++) {
        if (inp->z_order[i] == slot) {
            found = i;
            break;
        }
    }
    if (found < 0 || found == inp->z_count - 1) {
        return;
    }
    for (int i = found; i < inp->z_count - 1; i++) {
        inp->z_order[i] = inp->z_order[i + 1];
    }
    inp->z_order[inp->z_count - 1] = slot;
}

static void set_focus(stlxdm_input_t* inp, dm_client_t* clients,
                      int new_slot) {
    if (new_slot == inp->focused_slot) {
        return;
    }

    if (inp->focused_slot >= 0) {
        stlxgfx_dm_window_t* old_win = clients[inp->focused_slot].window;
        if (old_win && old_win->event_ring) {
            stlxgfx_event_t evt = {0};
            evt.type = STLXGFX_EVT_FOCUS_OUT;
            evt.window_id = old_win->window_id;
            stlxgfx_event_ring_write(old_win->event_ring, &evt);
        }
    }

    inp->focused_slot = new_slot;

    if (new_slot >= 0) {
        stlxgfx_dm_window_t* new_win = clients[new_slot].window;
        if (new_win && new_win->event_ring) {
            stlxgfx_event_t evt = {0};
            evt.type = STLXGFX_EVT_FOCUS_IN;
            evt.window_id = new_win->window_id;
            stlxgfx_event_ring_write(new_win->event_ring, &evt);
        }
    }
}

static void deliver_key(dm_client_t* clients, int slot,
                        const stlx_input_kbd_event_t* raw) {
    stlxgfx_dm_window_t* w = clients[slot].window;
    if (!w || !w->event_ring) {
        return;
    }

    stlxgfx_event_t evt = {0};
    evt.type = (raw->action == STLX_INPUT_KBD_ACTION_DOWN)
               ? STLXGFX_EVT_KEY_DOWN : STLXGFX_EVT_KEY_UP;
    evt.window_id = w->window_id;
    evt.key.usage = raw->usage;
    evt.key.modifiers = raw->modifiers;
    stlxgfx_event_ring_write(w->event_ring, &evt);
}

static void deliver_pointer(dm_client_t* clients, int slot,
                            stlxdm_input_t* inp, uint32_t type,
                            int16_t wheel, uint16_t buttons) {
    stlxgfx_dm_window_t* w = clients[slot].window;
    if (!w || !w->event_ring) {
        return;
    }

    stlxgfx_event_t evt = {0};
    evt.type = type;
    evt.window_id = w->window_id;
    evt.pointer.x = inp->ptr_x - w->x - STLXDM_DECOR_BORDER_W;
    evt.pointer.y = inp->ptr_y - w->y - STLXDM_DECOR_TITLEBAR_H;
    evt.pointer.wheel = wheel;
    evt.pointer.buttons = buttons;
    stlxgfx_event_ring_write(w->event_ring, &evt);
}

void stlxdm_input_process(stlxdm_input_t* inp, dm_client_t* clients,
                           int max_clients) {
    (void)max_clients;

    stlx_input_kbd_event_t kbd_buf[STLXDM_INPUT_MAX_RAW_PER_FRAME];
    if (inp->kbd_fd >= 0) {
        ssize_t n = read(inp->kbd_fd, kbd_buf, sizeof(kbd_buf));
        if (n > 0) {
            int count = (int)(n / (ssize_t)sizeof(stlx_input_kbd_event_t));
            for (int i = 0; i < count; i++) {
                if (inp->focused_slot >= 0) {
                    deliver_key(clients, inp->focused_slot, &kbd_buf[i]);
                }
            }
        }
    }

    stlx_input_mouse_event_t mouse_buf[STLXDM_INPUT_MAX_RAW_PER_FRAME];
    if (inp->mouse_fd >= 0) {
        ssize_t n = read(inp->mouse_fd, mouse_buf, sizeof(mouse_buf));
        if (n > 0) {
            int count = (int)(n / (ssize_t)sizeof(stlx_input_mouse_event_t));
            for (int i = 0; i < count; i++) {
                const stlx_input_mouse_event_t* me = &mouse_buf[i];

                if (me->flags & STLX_INPUT_MOUSE_FLAG_RELATIVE) {
                    inp->ptr_x += me->x_value;
                    inp->ptr_y += me->y_value;
                } else {
                    inp->ptr_x = me->x_value;
                    inp->ptr_y = me->y_value;
                }
                if (inp->ptr_x < 0) {
                    inp->ptr_x = 0;
                }
                if (inp->ptr_y < 0) {
                    inp->ptr_y = 0;
                }
                if (inp->ptr_x >= inp->fb_width) {
                    inp->ptr_x = inp->fb_width - 1;
                }
                if (inp->ptr_y >= inp->fb_height) {
                    inp->ptr_y = inp->fb_height - 1;
                }

                uint16_t prev_buttons = inp->capture_buttons;
                uint16_t cur_buttons = me->buttons;

                if ((cur_buttons & 1) && !(prev_buttons & 1)) {
                    int target = hit_test(inp, clients, inp->ptr_x, inp->ptr_y);
                    if (target >= 0) {
                        set_focus(inp, clients, target);
                        raise_slot(inp, target);
                        inp->capture_slot = target;
                    }
                }
                inp->capture_buttons = cur_buttons;

                int move_target = inp->capture_slot >= 0
                    ? inp->capture_slot
                    : hit_test(inp, clients, inp->ptr_x, inp->ptr_y);

                if (move_target >= 0 && (me->x_value != 0 || me->y_value != 0)) {
                    deliver_pointer(clients, move_target, inp,
                                    STLXGFX_EVT_POINTER_MOVE, 0, cur_buttons);
                }

                if ((cur_buttons & ~prev_buttons) != 0 && move_target >= 0) {
                    deliver_pointer(clients, move_target, inp,
                                    STLXGFX_EVT_POINTER_BTN_DOWN, 0, cur_buttons);
                }
                if ((prev_buttons & ~cur_buttons) != 0) {
                    int release_target = inp->capture_slot >= 0
                        ? inp->capture_slot : move_target;
                    if (release_target >= 0) {
                        deliver_pointer(clients, release_target, inp,
                                        STLXGFX_EVT_POINTER_BTN_UP, 0, cur_buttons);
                    }
                }

                if (cur_buttons == 0) {
                    inp->capture_slot = -1;
                }

                if (me->wheel != 0) {
                    int scroll_target = hit_test(inp, clients, inp->ptr_x, inp->ptr_y);
                    if (scroll_target >= 0) {
                        deliver_pointer(clients, scroll_target, inp,
                                        STLXGFX_EVT_POINTER_SCROLL,
                                        me->wheel, cur_buttons);
                    }
                }
            }
        }
    }
}

void stlxdm_input_draw_cursor(stlxdm_input_t* inp, stlxgfx_surface_t* surface) {
    int32_t x = inp->ptr_x;
    int32_t y = inp->ptr_y;

    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 18; col++) {
            char px = g_cursor_shape[row][col];
            if (px == 'X' || px == '.') {
                int32_t sx = x + col + 1;
                int32_t sy = y + row + 1;
                stlxgfx_fill_rect(surface, sx, sy, 1, 1, 0x80000000);
            }
        }
    }

    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 18; col++) {
            char px = g_cursor_shape[row][col];
            if (px == 'X') {
                stlxgfx_fill_rect(surface, x + col, y + row, 1, 1, 0xFF000000);
            } else if (px == '.') {
                stlxgfx_fill_rect(surface, x + col, y + row, 1, 1, 0xFFFFFFFF);
            }
        }
    }
}
