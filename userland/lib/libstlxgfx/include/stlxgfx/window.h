#ifndef STLXGFX_WINDOW_H
#define STLXGFX_WINDOW_H

#include <stlxgfx/fb.h>
#include <stdatomic.h>

#define STLXGFX_WINDOW_SURFACE_FD 3
#define STLXGFX_WINDOW_SYNC_FD   4

typedef struct {
    atomic_uint_least32_t front_index;
    atomic_uint_least32_t frame_ready;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    atomic_uint_least32_t close_requested;
} stlxgfx_window_sync_t;

/* --- Client-side API --- */

typedef struct {
    stlxgfx_window_sync_t* sync;
    uint8_t* surface_buf;
    size_t   surface_size;
    stlxgfx_surface_t* back;
} stlxgfx_window_t;

stlxgfx_window_t* stlxgfx_window_open(int surface_fd, int sync_fd);
void stlxgfx_window_close(stlxgfx_window_t* window);
stlxgfx_surface_t* stlxgfx_window_back_buffer(stlxgfx_window_t* window);
int stlxgfx_window_present(stlxgfx_window_t* window);
int stlxgfx_window_should_close(stlxgfx_window_t* window);

/* --- DM-side API --- */

typedef struct {
    stlxgfx_window_sync_t* sync;
    uint8_t* surface_buf;
    size_t   surface_size;
    int      surface_fd;
    int      sync_fd;
    stlxgfx_surface_t* front;
    int32_t  x, y;
} stlxgfx_dm_window_t;

stlxgfx_dm_window_t* stlxgfx_dm_create_window(
    uint32_t width, uint32_t height,
    int32_t x, int32_t y,
    const stlxgfx_fb_t* fb);
void stlxgfx_dm_destroy_window(stlxgfx_dm_window_t* window);
int stlxgfx_dm_sync(stlxgfx_dm_window_t* window);
stlxgfx_surface_t* stlxgfx_dm_front_buffer(stlxgfx_dm_window_t* window);

#endif /* STLXGFX_WINDOW_H */
