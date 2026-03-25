#ifndef STLXDM_INPUT_H
#define STLXDM_INPUT_H

#include <stlxgfx/window.h>
#include <stlxgfx/event.h>
#include <stlx/input.h>
#include <stdint.h>

#define STLXDM_INPUT_MAX_RAW_PER_FRAME 64

typedef struct {
    int kbd_fd;
    int mouse_fd;

    int32_t ptr_x;
    int32_t ptr_y;
    int32_t fb_width;
    int32_t fb_height;

    int focused_slot;
    int capture_slot;
    uint16_t capture_buttons;

    int drag_slot;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
    int close_hover_slot;
    int close_press_slot;

    int z_order[STLXGFX_DM_MAX_CLIENTS];
    int z_count;

    int spawn_terminal_requested;
} stlxdm_input_t;

typedef struct {
    int fd;
    stlxgfx_dm_window_t* window;
} dm_client_t;

void stlxdm_input_init(stlxdm_input_t* inp, int32_t fb_w, int32_t fb_h);
void stlxdm_input_process(stlxdm_input_t* inp, dm_client_t* clients, int max_clients);
void stlxdm_input_add_window(stlxdm_input_t* inp, int slot);
void stlxdm_input_add_window_with_focus(stlxdm_input_t* inp, int slot,
                                         dm_client_t* clients);
void stlxdm_input_remove_window(stlxdm_input_t* inp, int slot);
void stlxdm_input_draw_cursor(stlxdm_input_t* inp, stlxgfx_surface_t* surface);
int  stlxdm_input_z_order(const stlxdm_input_t* inp, int idx);

#endif /* STLXDM_INPUT_H */
