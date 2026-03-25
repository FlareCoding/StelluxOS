#define _POSIX_C_SOURCE 199309L
#include "stlxdm_decor.h"
#include "stlxdm_input.h"
#include <stlxgfx/surface.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    HIT_NONE,
    HIT_CLOSE_BUTTON,
    HIT_TITLE_BAR,
    HIT_CLIENT
} hit_zone_t;

static void set_focus(stlxdm_input_t* inp, dm_client_t* clients, int new_slot);

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
    inp->drag_slot = -1;
    inp->close_hover_slot = -1;
    inp->close_press_slot = -1;
    inp->z_count = 0;
}

void stlxdm_input_add_window(stlxdm_input_t* inp, int slot,
                              dm_client_t* clients) {
    for (int i = 0; i < inp->z_count; i++) {
        if (inp->z_order[i] == slot) {
            return;
        }
    }
    if (inp->z_count >= STLXGFX_DM_MAX_CLIENTS) {
        return;
    }
    inp->z_order[inp->z_count++] = slot;
    set_focus(inp, clients, slot);
}

void stlxdm_input_remove_window(stlxdm_input_t* inp, int slot) {
    int found = -1;
    for (int i = 0; i < inp->z_count; i++) {
        if (inp->z_order[i] == slot) { found = i; break; }
    }
    if (found >= 0) {
        for (int i = found; i < inp->z_count - 1; i++)
            inp->z_order[i] = inp->z_order[i + 1];
        inp->z_count--;
    }
    if (inp->focused_slot == slot)
        inp->focused_slot = inp->z_count > 0 ? inp->z_order[inp->z_count - 1] : -1;
    if (inp->capture_slot == slot) {
        inp->capture_slot = -1;
        inp->capture_buttons = 0;
    }
    if (inp->drag_slot == slot)
        inp->drag_slot = -1;
    if (inp->close_hover_slot == slot)
        inp->close_hover_slot = -1;
    if (inp->close_press_slot == slot)
        inp->close_press_slot = -1;
}

int stlxdm_input_z_order(const stlxdm_input_t* inp, int idx) {
    if (idx < 0 || idx >= inp->z_count) {
        return -1;
    }
    return inp->z_order[idx];
}

static hit_zone_t hit_test_zone(const stlxdm_input_t* inp,
                                const dm_client_t* clients,
                                int32_t px, int32_t py, int* out_slot) {
    *out_slot = -1;
    for (int i = inp->z_count - 1; i >= 0; i--) {
        int slot = inp->z_order[i];
        stlxgfx_dm_window_t* w = clients[slot].window;
        if (!w) {
            continue;
        }

        int32_t outer_w = (int32_t)w->width + 2 * STLXDM_BORDER_WIDTH;
        int32_t outer_h = (int32_t)w->height + STLXDM_TITLE_HEIGHT + STLXDM_BORDER_WIDTH;

        if (px < w->x || px >= w->x + outer_w ||
            py < w->y || py >= w->y + outer_h)
            continue;

        *out_slot = slot;

        if (py < w->y + STLXDM_TITLE_HEIGHT) {
            int32_t ccx = w->x + outer_w - STLXDM_CLOSE_BTN_MARGIN
                        - STLXDM_CLOSE_BTN_RADIUS - STLXDM_BORDER_WIDTH;
            int32_t ccy = w->y + STLXDM_TITLE_HEIGHT / 2;
            int32_t dx = px - ccx;
            int32_t dy = py - ccy;
            if (dx * dx + dy * dy <= STLXDM_CLOSE_BTN_RADIUS * STLXDM_CLOSE_BTN_RADIUS) {
                return HIT_CLOSE_BUTTON;
            }
            return HIT_TITLE_BAR;
        }
        return HIT_CLIENT;
    }
    return HIT_NONE;
}

static void raise_slot(stlxdm_input_t* inp, int slot) {
    int found = -1;
    for (int i = 0; i < inp->z_count; i++) {
        if (inp->z_order[i] == slot) { found = i; break; }
    }
    if (found < 0 || found == inp->z_count - 1) {
        return;
    }
    for (int i = found; i < inp->z_count - 1; i++)
        inp->z_order[i] = inp->z_order[i + 1];
    inp->z_order[inp->z_count - 1] = slot;
}

static void set_focus(stlxdm_input_t* inp, dm_client_t* clients, int new_slot) {
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
    evt.pointer.x = inp->ptr_x - w->x - STLXDM_BORDER_WIDTH;
    evt.pointer.y = inp->ptr_y - w->y - STLXDM_TITLE_HEIGHT;
    evt.pointer.wheel = wheel;
    evt.pointer.buttons = buttons;
    stlxgfx_event_ring_write(w->event_ring, &evt);
}

static int is_global_shortcut(const stlx_input_kbd_event_t* evt,
                              stlxdm_input_t* inp) {
    if (evt->action != STLX_INPUT_KBD_ACTION_DOWN) {
        return 0;
    }

    uint8_t ctrl = STLX_INPUT_MOD_LCTRL | STLX_INPUT_MOD_RCTRL;
    uint8_t alt  = STLX_INPUT_MOD_LALT  | STLX_INPUT_MOD_RALT;

    int has_ctrl = (evt->modifiers & ctrl) != 0;
    int has_alt  = (evt->modifiers & alt)  != 0;

    if (has_ctrl && has_alt && evt->usage == 0x17) {
        inp->spawn_terminal_requested = 1;
        return 1;
    }

    return 0;
}

void stlxdm_input_process(stlxdm_input_t* inp, dm_client_t* clients,
                           int max_clients) {
    (void)max_clients;

    inp->spawn_terminal_requested = 0;

    stlx_input_kbd_event_t kbd_buf[STLXDM_INPUT_MAX_RAW_PER_FRAME];
    if (inp->kbd_fd >= 0) {
        ssize_t n = read(inp->kbd_fd, kbd_buf, sizeof(kbd_buf));
        if (n > 0) {
            int count = (int)(n / (ssize_t)sizeof(stlx_input_kbd_event_t));
            for (int i = 0; i < count; i++) {
                if (is_global_shortcut(&kbd_buf[i], inp)) {
                    continue;
                }
                if (inp->focused_slot >= 0)
                    deliver_key(clients, inp->focused_slot, &kbd_buf[i]);
            }
        }
    }

    inp->close_hover_slot = -1;

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

                if (inp->drag_slot >= 0) {
                    stlxgfx_dm_window_t* dw = clients[inp->drag_slot].window;
                    if (dw) {
                        dw->x = inp->ptr_x - inp->drag_offset_x;
                        dw->y = inp->ptr_y - inp->drag_offset_y;
                        if (dw->y < (int32_t)STLXDM_BAR_HEIGHT) {
                            dw->y = (int32_t)STLXDM_BAR_HEIGHT;
                        }
                    }
                    if (cur_buttons == 0) {
                        inp->drag_slot = -1;
                        inp->capture_slot = -1;
                    }
                    inp->capture_buttons = cur_buttons;
                    continue;
                }

                if ((cur_buttons & 1) && !(prev_buttons & 1)) {
                    int target = -1;
                    hit_zone_t zone = hit_test_zone(inp, clients,
                                                     inp->ptr_x, inp->ptr_y, &target);
                    if (target >= 0) {
                        set_focus(inp, clients, target);
                        raise_slot(inp, target);
                        inp->capture_slot = target;

                        if (zone == HIT_TITLE_BAR) {
                            stlxgfx_dm_window_t* tw = clients[target].window;
                            if (tw) {
                                inp->drag_slot = target;
                                inp->drag_offset_x = inp->ptr_x - tw->x;
                                inp->drag_offset_y = inp->ptr_y - tw->y;
                            }
                        } else if (zone == HIT_CLOSE_BUTTON) {
                            inp->close_press_slot = target;
                        } else if (zone == HIT_CLIENT) {
                            deliver_pointer(clients, target, inp,
                                            STLXGFX_EVT_POINTER_BTN_DOWN,
                                            0, cur_buttons);
                        }
                    }
                } else if ((prev_buttons & 1) && !(cur_buttons & 1)) {
                    if (inp->close_press_slot >= 0) {
                        int rel_slot = -1;
                        hit_zone_t rel_zone = hit_test_zone(inp, clients,
                            inp->ptr_x, inp->ptr_y, &rel_slot);
                        if (rel_zone == HIT_CLOSE_BUTTON &&
                            rel_slot == inp->close_press_slot) {
                            stlxgfx_dm_window_t* cw = clients[rel_slot].window;
                            if (cw && cw->sync) {
                                atomic_store_explicit(&cw->sync->close_requested,
                                                      1, memory_order_release);
                            }
                        }
                        inp->close_press_slot = -1;
                    } else if (inp->capture_slot >= 0) {
                        deliver_pointer(clients, inp->capture_slot, inp,
                                        STLXGFX_EVT_POINTER_BTN_UP,
                                        0, cur_buttons);
                    }
                    if (cur_buttons == 0) {
                        inp->capture_slot = -1;
                    }
                } else {
                    int move_target = inp->capture_slot >= 0
                        ? inp->capture_slot
                        : -1;
                    if (move_target < 0) {
                        int dummy;
                        hit_test_zone(inp, clients,
                                      inp->ptr_x, inp->ptr_y, &dummy);
                        move_target = dummy;
                    }
                    if (move_target >= 0 && (me->x_value != 0 || me->y_value != 0)) {
                        deliver_pointer(clients, move_target, inp,
                                        STLXGFX_EVT_POINTER_MOVE,
                                        0, cur_buttons);
                    }
                }

                inp->capture_buttons = cur_buttons;

                if (me->wheel != 0) {
                    int scroll_slot = -1;
                    hit_test_zone(inp, clients,
                                  inp->ptr_x, inp->ptr_y, &scroll_slot);
                    if (scroll_slot >= 0) {
                        deliver_pointer(clients, scroll_slot, inp,
                                        STLXGFX_EVT_POINTER_SCROLL,
                                        me->wheel, cur_buttons);
                    }
                }
            }
        }
    }

    if (inp->drag_slot < 0 && inp->capture_buttons == 0) {
        int hover_slot = -1;
        hit_zone_t zone = hit_test_zone(inp, clients,
                                         inp->ptr_x, inp->ptr_y, &hover_slot);
        if (zone == HIT_CLOSE_BUTTON && hover_slot == inp->focused_slot) {
            inp->close_hover_slot = hover_slot;
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
                stlxgfx_fill_rect(surface, x + col + 1, y + row + 1,
                                   1, 1, 0x80000000);
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
