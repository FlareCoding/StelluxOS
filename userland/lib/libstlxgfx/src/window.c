#define _GNU_SOURCE
#include <stlxgfx/window.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* --- DM-side implementation --- */

stlxgfx_dm_window_t* stlxgfx_dm_create_window(uint32_t width, uint32_t height,
                                               int32_t x, int32_t y,
                                               const stlxgfx_fb_t* fb) {
    if (!fb || width == 0 || height == 0) {
        return NULL;
    }

    uint32_t bytes_pp = fb->bpp / 8;
    uint32_t pitch = width * bytes_pp;
    size_t single_buf = (size_t)pitch * height;
    size_t surface_size = single_buf * 2;
    size_t sync_size = sizeof(stlxgfx_window_sync_t);

    int surface_fd = memfd_create("stlxgfx_surface", 0);
    if (surface_fd < 0) {
        return NULL;
    }
    if (ftruncate(surface_fd, (off_t)surface_size) < 0) {
        close(surface_fd);
        return NULL;
    }

    int sync_fd = memfd_create("stlxgfx_sync", 0);
    if (sync_fd < 0) {
        close(surface_fd);
        return NULL;
    }
    if (ftruncate(sync_fd, (off_t)sync_size) < 0) {
        close(sync_fd);
        close(surface_fd);
        return NULL;
    }

    uint8_t* surface_buf = mmap(NULL, surface_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                surface_fd, 0);
    if (surface_buf == MAP_FAILED) {
        close(sync_fd);
        close(surface_fd);
        return NULL;
    }

    stlxgfx_window_sync_t* sync = mmap(NULL, sync_size,
                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                        sync_fd, 0);
    if (sync == MAP_FAILED) {
        munmap(surface_buf, surface_size);
        close(sync_fd);
        close(surface_fd);
        return NULL;
    }

    memset(surface_buf, 0, surface_size);

    atomic_store_explicit(&sync->front_index, 0, memory_order_relaxed);
    atomic_store_explicit(&sync->frame_ready, 0, memory_order_relaxed);
    sync->width       = width;
    sync->height      = height;
    sync->pitch       = pitch;
    sync->bpp         = fb->bpp;
    sync->red_shift   = fb->red_shift;
    sync->green_shift = fb->green_shift;
    sync->blue_shift  = fb->blue_shift;
    atomic_store_explicit(&sync->close_requested, 0, memory_order_relaxed);

    stlxgfx_dm_window_t* win = malloc(sizeof(stlxgfx_dm_window_t));
    if (!win) {
        munmap(sync, sync_size);
        munmap(surface_buf, surface_size);
        close(sync_fd);
        close(surface_fd);
        return NULL;
    }

    win->sync         = sync;
    win->surface_buf  = surface_buf;
    win->surface_size = surface_size;
    win->surface_fd   = surface_fd;
    win->sync_fd      = sync_fd;
    win->x            = x;
    win->y            = y;

    win->front = stlxgfx_surface_from_buffer(
        surface_buf,
        width, height, pitch, fb->bpp,
        fb->red_shift, fb->green_shift, fb->blue_shift);
    if (!win->front) {
        munmap(sync, sync_size);
        munmap(surface_buf, surface_size);
        close(sync_fd);
        close(surface_fd);
        free(win);
        return NULL;
    }

    return win;
}

void stlxgfx_dm_destroy_window(stlxgfx_dm_window_t* window) {
    if (!window) {
        return;
    }
    if (window->front) {
        stlxgfx_destroy_surface(window->front);
    }
    if (window->sync) {
        munmap(window->sync, sizeof(stlxgfx_window_sync_t));
    }
    if (window->surface_buf) {
        munmap(window->surface_buf, window->surface_size);
    }
    if (window->surface_fd >= 0) {
        close(window->surface_fd);
    }
    if (window->sync_fd >= 0) {
        close(window->sync_fd);
    }
    free(window);
}

int stlxgfx_dm_sync(stlxgfx_dm_window_t* window) {
    if (!window || !window->sync) {
        return 0;
    }

    uint32_t ready = atomic_load_explicit(&window->sync->frame_ready,
                                          memory_order_acquire);
    if (!ready) {
        return 0;
    }

    uint32_t old_front = atomic_load_explicit(&window->sync->front_index,
                                              memory_order_relaxed);
    uint32_t new_front = 1 - old_front;

    atomic_store_explicit(&window->sync->front_index, new_front,
                          memory_order_relaxed);
    atomic_store_explicit(&window->sync->frame_ready, 0,
                          memory_order_release);

    size_t single_buf = (size_t)window->sync->pitch * window->sync->height;
    window->front->pixels = window->surface_buf + new_front * single_buf;

    return 1;
}

stlxgfx_surface_t* stlxgfx_dm_front_buffer(stlxgfx_dm_window_t* window) {
    if (!window) {
        return NULL;
    }
    return window->front;
}

/* --- Client-side implementation --- */

stlxgfx_window_t* stlxgfx_window_open(int surface_fd, int sync_fd) {
    stlxgfx_window_sync_t* sync = mmap(NULL, sizeof(stlxgfx_window_sync_t),
                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                        sync_fd, 0);
    if (sync == MAP_FAILED) {
        return NULL;
    }

    size_t single_buf = (size_t)sync->pitch * sync->height;
    size_t surface_size = single_buf * 2;

    uint8_t* surface_buf = mmap(NULL, surface_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                surface_fd, 0);
    if (surface_buf == MAP_FAILED) {
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        return NULL;
    }

    stlxgfx_window_t* win = malloc(sizeof(stlxgfx_window_t));
    if (!win) {
        munmap(surface_buf, surface_size);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        return NULL;
    }

    win->sync         = sync;
    win->surface_buf  = surface_buf;
    win->surface_size = surface_size;

    uint32_t fi = atomic_load_explicit(&sync->front_index, memory_order_acquire);
    win->back = stlxgfx_surface_from_buffer(
        surface_buf + (1 - fi) * single_buf,
        sync->width, sync->height, sync->pitch, sync->bpp,
        sync->red_shift, sync->green_shift, sync->blue_shift);
    if (!win->back) {
        munmap(surface_buf, surface_size);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        free(win);
        return NULL;
    }

    return win;
}

void stlxgfx_window_close(stlxgfx_window_t* window) {
    if (!window) {
        return;
    }
    if (window->back) {
        stlxgfx_destroy_surface(window->back);
    }
    if (window->surface_buf) {
        munmap(window->surface_buf, window->surface_size);
    }
    if (window->sync) {
        munmap(window->sync, sizeof(stlxgfx_window_sync_t));
    }
    free(window);
}

stlxgfx_surface_t* stlxgfx_window_back_buffer(stlxgfx_window_t* window) {
    if (!window || !window->sync || !window->back) {
        return NULL;
    }

    uint32_t ready = atomic_load_explicit(&window->sync->frame_ready,
                                          memory_order_acquire);
    if (ready) {
        return NULL;
    }

    uint32_t fi = atomic_load_explicit(&window->sync->front_index,
                                       memory_order_acquire);
    size_t single_buf = (size_t)window->sync->pitch * window->sync->height;
    window->back->pixels = window->surface_buf + (1 - fi) * single_buf;

    return window->back;
}

int stlxgfx_window_present(stlxgfx_window_t* window) {
    if (!window || !window->sync) {
        return -1;
    }
    atomic_store_explicit(&window->sync->frame_ready, 1,
                          memory_order_release);
    return 0;
}

int stlxgfx_window_should_close(stlxgfx_window_t* window) {
    if (!window || !window->sync) {
        return 1;
    }
    return atomic_load_explicit(&window->sync->close_requested,
                                memory_order_acquire) != 0;
}
