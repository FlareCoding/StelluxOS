#define _GNU_SOURCE
#include <stlxgfx/window.h>
#include <stlxgfx/font.h>

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* --- I/O helpers --- */

static int read_full(int fd, void* buf, size_t count) {
    uint8_t* p = (uint8_t*)buf;
    size_t remaining = count;
    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        p += (size_t)n;
        remaining -= (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const void* buf, size_t count) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t remaining = count;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) {
            return -1;
        }
        p += (size_t)n;
        remaining -= (size_t)n;
    }
    return 0;
}

static int send_message(int fd, uint32_t type, uint32_t seq,
                        const void* payload, uint32_t payload_size) {
    stlxgfx_msg_header_t hdr;
    hdr.protocol_version = STLXGFX_PROTOCOL_VERSION;
    hdr.message_type = type;
    hdr.sequence_number = seq;
    hdr.payload_size = payload_size;
    hdr.flags = 0;
    if (write_full(fd, &hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (payload_size > 0 && payload) {
        if (write_full(fd, payload, payload_size) != 0) {
            return -1;
        }
    }
    return 0;
}

/* --- Client-side implementation --- */

static int stlxgfx_connect(const char* socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static stlxgfx_window_t* create_window_internal(int conn_fd, uint32_t width,
                                                  uint32_t height, const char* title,
                                                  int owns_connection) {
    if (conn_fd < 0 || width == 0 || height == 0) {
        return NULL;
    }

    stlxgfx_create_window_req_t req;
    memset(&req, 0, sizeof(req));
    req.width = width;
    req.height = height;
    if (title) {
        size_t len = strlen(title);
        if (len > 255) {
            len = 255;
        }
        memcpy(req.title, title, len);
        req.title_length = (uint32_t)len;
    }

    if (send_message(conn_fd, STLXGFX_MSG_CREATE_WINDOW_REQ, 1,
                     &req, sizeof(req)) != 0) {
        return NULL;
    }

    stlxgfx_msg_header_t resp_hdr;
    if (read_full(conn_fd, &resp_hdr, sizeof(resp_hdr)) != 0) {
        return NULL;
    }
    if (resp_hdr.protocol_version != STLXGFX_PROTOCOL_VERSION) {
        return NULL;
    }
    if (resp_hdr.message_type != STLXGFX_MSG_CREATE_WINDOW_RESP) {
        return NULL;
    }

    stlxgfx_create_window_resp_t resp;
    if (read_full(conn_fd, &resp, sizeof(resp)) != 0) {
        return NULL;
    }
    if (resp.result_code != 0) {
        return NULL;
    }

    char surface_path[128];
    char sync_path[128];
    char events_path[128];
    snprintf(surface_path, sizeof(surface_path), "/dev/shm/%s", resp.surface_name);
    snprintf(sync_path, sizeof(sync_path), "/dev/shm/%s", resp.sync_name);
    snprintf(events_path, sizeof(events_path), "/dev/shm/%s", resp.events_name);
    int sync_fd = open(sync_path, O_RDWR);
    if (sync_fd < 0) {
        return NULL;
    }
    stlxgfx_window_sync_t* sync = mmap(NULL, sizeof(stlxgfx_window_sync_t),
                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                        sync_fd, 0);
    if (sync == MAP_FAILED) {
        close(sync_fd);
        return NULL;
    }

    int events_fd = open(events_path, O_RDWR);
    if (events_fd < 0) {
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        return NULL;
    }
    stlxgfx_event_ring_t* event_ring = mmap(NULL, sizeof(stlxgfx_event_ring_t),
                                             PROT_READ | PROT_WRITE, MAP_SHARED,
                                             events_fd, 0);
    if (event_ring == MAP_FAILED) {
        close(events_fd);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        return NULL;
    }

    size_t single_buf = (size_t)resp.pitch * resp.height;
    size_t surface_size = single_buf * 3;

    int surface_fd = open(surface_path, O_RDWR);
    if (surface_fd < 0) {
        munmap(event_ring, sizeof(stlxgfx_event_ring_t));
        close(events_fd);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        return NULL;
    }
    uint8_t* surface_buf = mmap(NULL, surface_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                surface_fd, 0);
    if (surface_buf == MAP_FAILED) {
        close(surface_fd);
        munmap(event_ring, sizeof(stlxgfx_event_ring_t));
        close(events_fd);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        return NULL;
    }

    stlxgfx_window_t* win = malloc(sizeof(stlxgfx_window_t));
    if (!win) {
        munmap(surface_buf, surface_size);
        close(surface_fd);
        munmap(event_ring, sizeof(stlxgfx_event_ring_t));
        close(events_fd);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        return NULL;
    }

    win->sync = sync;
    win->event_ring = event_ring;
    win->surface_buf = surface_buf;
    win->surface_size = surface_size;
    win->surface_fd = surface_fd;
    win->sync_fd = sync_fd;
    win->events_fd = events_fd;
    win->window_id = resp.window_id;
    win->width = resp.width;
    win->height = resp.height;
    win->pitch = resp.pitch;
    win->bpp = resp.bpp;
    win->red_shift = resp.red_shift;
    win->green_shift = resp.green_shift;
    win->blue_shift = resp.blue_shift;
    win->conn_fd = owns_connection ? conn_fd : -1;
    win->open = 1;

    uint32_t bi = atomic_load_explicit(&sync->back_index, memory_order_acquire);
    win->back = stlxgfx_surface_from_buffer(
        surface_buf + bi * single_buf,
        resp.width, resp.height, resp.pitch, resp.bpp,
        resp.red_shift, resp.green_shift, resp.blue_shift);
    if (!win->back) {
        munmap(surface_buf, surface_size);
        close(surface_fd);
        munmap(event_ring, sizeof(stlxgfx_event_ring_t));
        close(events_fd);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        free(win);
        return NULL;
    }

    return win;
}

stlxgfx_window_t* stlxgfx_create_window(uint32_t width, uint32_t height,
                                          const char* title) {
    stlxgfx_font_init(STLXGFX_FONT_PATH);

    int conn_fd = stlxgfx_connect(STLXGFX_DM_SOCKET_PATH);
    if (conn_fd < 0) {
        return NULL;
    }

    stlxgfx_window_t* win = create_window_internal(conn_fd, width, height, title, 1);
    if (!win) {
        close(conn_fd);
        return NULL;
    }
    return win;
}

static void window_cleanup(stlxgfx_window_t* window) {
    if (!window) {
        return;
    }

    int owned_conn = window->conn_fd;

    if (owned_conn >= 0) {
        stlxgfx_destroy_window_req_t req = { .window_id = window->window_id };
        send_message(owned_conn, STLXGFX_MSG_DESTROY_WINDOW_REQ, 0,
                     &req, sizeof(req));
    }
    if (window->back)
        stlxgfx_destroy_surface(window->back);
    if (window->event_ring)
        munmap(window->event_ring, sizeof(stlxgfx_event_ring_t));
    if (window->surface_buf)
        munmap(window->surface_buf, window->surface_size);
    if (window->events_fd >= 0)
        close(window->events_fd);
    if (window->surface_fd >= 0)
        close(window->surface_fd);
    if (window->sync)
        munmap(window->sync, sizeof(stlxgfx_window_sync_t));
    if (window->sync_fd >= 0)
        close(window->sync_fd);
    if (owned_conn >= 0)
        close(owned_conn);

    window->open = 0;
}

void stlxgfx_window_destroy(stlxgfx_window_t* window) {
    if (!window) {
        return;
    }
    window_cleanup(window);
    free(window);
}

int stlxgfx_window_is_open(stlxgfx_window_t* window) {
    if (!window) {
        return 0;
    }
    return window->open;
}

stlxgfx_surface_t* stlxgfx_window_back_buffer(stlxgfx_window_t* window) {
    if (!window || !window->sync || !window->back || !window->open) {
        return NULL;
    }
    uint32_t bi = atomic_load_explicit(&window->sync->back_index,
                                       memory_order_acquire);
    size_t single_buf = (size_t)window->pitch * window->height;
    window->back->pixels = window->surface_buf + bi * single_buf;
    return window->back;
}

int stlxgfx_window_swap_buffers(stlxgfx_window_t* window) {
    if (!window || !window->sync || !window->open) {
        return -1;
    }
    stlxgfx_window_sync_t* s = window->sync;

    if (atomic_load_explicit(&s->swap_pending, memory_order_acquire)) {
        return -1;
    }

    uint32_t bi = atomic_load_explicit(&s->back_index, memory_order_relaxed);
    atomic_store_explicit(&s->ready_index, bi, memory_order_relaxed);
    atomic_store_explicit(&s->frame_ready, 1, memory_order_relaxed);
    atomic_store_explicit(&s->swap_pending, 1, memory_order_release);

    uint32_t next = (bi + 1) % 3;
    uint32_t fi = atomic_load_explicit(&s->front_index, memory_order_acquire);
    if (next == fi) {
        next = (next + 1) % 3;
    }
    atomic_store_explicit(&s->back_index, next, memory_order_release);

    return 0;
}

int stlxgfx_window_next_event(stlxgfx_window_t* window,
                               stlxgfx_event_t* event) {
    if (!window || !window->event_ring || !event) {
        return 0;
    }

    if (window->sync &&
        atomic_load_explicit(&window->sync->close_requested,
                             memory_order_acquire)) {
        stlxgfx_event_t close_evt;
        memset(&close_evt, 0, sizeof(close_evt));
        close_evt.type = STLXGFX_EVT_CLOSE_REQUESTED;
        close_evt.window_id = window->window_id;
        *event = close_evt;
        atomic_store_explicit(&window->sync->close_requested, 0,
                              memory_order_release);
        return 1;
    }

    return stlxgfx_event_ring_read(window->event_ring, event);
}

/* --- DM-side implementation --- */

int stlxgfx_dm_listen(const char* socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "stlxgfx_dm_listen: socket() failed (errno=%d)\r\n", errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    unlink(socket_path);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "stlxgfx_dm_listen: bind(%s) failed (errno=%d)\r\n", socket_path, errno);
        close(fd);
        return -1;
    }
    if (listen(fd, STLXGFX_DM_MAX_CLIENTS) < 0) {
        fprintf(stderr, "stlxgfx_dm_listen: listen() failed (errno=%d)\r\n", errno);
        close(fd);
        return -1;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

int stlxgfx_dm_accept(int listen_fd) {
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

int stlxgfx_dm_read_request(int client_fd, stlxgfx_msg_header_t* header,
                             void* payload, size_t max_payload) {
    ssize_t n = read(client_fd, header, sizeof(*header));
    if (n < 0) {
        if (errno == EAGAIN) {
            return 0;
        }
        return -1;
    }
    if (n == 0) {
        return -1;
    }
    if ((size_t)n < sizeof(*header)) {
        uint8_t* p = (uint8_t*)header + n;
        size_t remaining = sizeof(*header) - (size_t)n;
        if (read_full(client_fd, p, remaining) != 0) {
            return -1;
        }
    }

    if (header->protocol_version != STLXGFX_PROTOCOL_VERSION) {
        return -1;
    }
    if (header->payload_size > 0) {
        if (header->payload_size > max_payload || !payload) {
            return -1;
        }
        if (read_full(client_fd, payload, header->payload_size) != 0) {
            return -1;
        }
    }
    return 1;
}

static uint32_t g_next_window_id = 1;
static int32_t  g_cascade_offset = 0;

stlxgfx_dm_window_t* stlxgfx_dm_handle_create_window(
    int client_fd, const stlxgfx_msg_header_t* req_header,
    const stlxgfx_create_window_req_t* req, const stlxgfx_fb_t* fb) {
    if (!req || !fb || req->width == 0 || req->height == 0) {
        return NULL;
    }

    uint32_t wid = g_next_window_id++;
    uint32_t bytes_pp = fb->bpp / 8;
    uint32_t pitch = req->width * bytes_pp;
    size_t single_buf = (size_t)pitch * req->height;
    size_t surface_size = single_buf * 3;

    char surface_name[64];
    char sync_name[64];
    char events_name[64];
    snprintf(surface_name, sizeof(surface_name), "stlxgfx_surface_%u", wid);
    snprintf(sync_name, sizeof(sync_name), "stlxgfx_sync_%u", wid);
    snprintf(events_name, sizeof(events_name), "stlxgfx_events_%u", wid);

    char surface_path[128];
    char sync_path[128];
    char events_path[128];
    snprintf(surface_path, sizeof(surface_path), "/dev/shm/%s", surface_name);
    snprintf(sync_path, sizeof(sync_path), "/dev/shm/%s", sync_name);
    snprintf(events_path, sizeof(events_path), "/dev/shm/%s", events_name);

    int surface_fd = open(surface_path, O_CREAT | O_RDWR, 0);
    if (surface_fd < 0) {
        return NULL;
    }
    if (ftruncate(surface_fd, (off_t)surface_size) < 0) {
        close(surface_fd);
        return NULL;
    }
    uint8_t* surface_buf = mmap(NULL, surface_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                surface_fd, 0);
    if (surface_buf == MAP_FAILED) {
        close(surface_fd);
        return NULL;
    }
    memset(surface_buf, 0, surface_size);

    int sync_fd = open(sync_path, O_CREAT | O_RDWR, 0);
    if (sync_fd < 0) {
        munmap(surface_buf, surface_size);
        close(surface_fd);
        return NULL;
    }
    if (ftruncate(sync_fd, (off_t)sizeof(stlxgfx_window_sync_t)) < 0) {
        close(sync_fd);
        munmap(surface_buf, surface_size);
        close(surface_fd);
        return NULL;
    }
    stlxgfx_window_sync_t* sync = mmap(NULL, sizeof(stlxgfx_window_sync_t),
                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                        sync_fd, 0);
    if (sync == MAP_FAILED) {
        close(sync_fd);
        munmap(surface_buf, surface_size);
        close(surface_fd);
        return NULL;
    }

    int events_fd = open(events_path, O_CREAT | O_RDWR, 0);
    if (events_fd < 0) {
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        munmap(surface_buf, surface_size);
        close(surface_fd);
        return NULL;
    }
    if (ftruncate(events_fd, (off_t)sizeof(stlxgfx_event_ring_t)) < 0) {
        close(events_fd);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        munmap(surface_buf, surface_size);
        close(surface_fd);
        return NULL;
    }
    stlxgfx_event_ring_t* event_ring = mmap(NULL, sizeof(stlxgfx_event_ring_t),
                                             PROT_READ | PROT_WRITE, MAP_SHARED,
                                             events_fd, 0);
    if (event_ring == MAP_FAILED) {
        close(events_fd);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        munmap(surface_buf, surface_size);
        close(surface_fd);
        return NULL;
    }
    stlxgfx_event_ring_init(event_ring);

    atomic_store_explicit(&sync->front_index, 0, memory_order_relaxed);
    atomic_store_explicit(&sync->back_index, 1, memory_order_relaxed);
    atomic_store_explicit(&sync->ready_index, 2, memory_order_relaxed);
    atomic_store_explicit(&sync->frame_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&sync->dm_consuming, 0, memory_order_relaxed);
    atomic_store_explicit(&sync->swap_pending, 0, memory_order_relaxed);
    atomic_store_explicit(&sync->close_requested, 0, memory_order_release);

    stlxgfx_dm_window_t* win = malloc(sizeof(stlxgfx_dm_window_t));
    if (!win) {
        munmap(event_ring, sizeof(stlxgfx_event_ring_t));
        close(events_fd);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        munmap(surface_buf, surface_size);
        close(surface_fd);
        return NULL;
    }

    win->front = stlxgfx_surface_from_buffer(
        surface_buf,
        req->width, req->height, pitch, fb->bpp,
        fb->red_shift, fb->green_shift, fb->blue_shift);
    if (!win->front) {
        munmap(event_ring, sizeof(stlxgfx_event_ring_t));
        close(events_fd);
        munmap(sync, sizeof(stlxgfx_window_sync_t));
        close(sync_fd);
        munmap(surface_buf, surface_size);
        close(surface_fd);
        free(win);
        return NULL;
    }

    win->sync = sync;
    win->event_ring = event_ring;
    win->surface_buf = surface_buf;
    win->surface_size = surface_size;
    win->surface_fd = surface_fd;
    win->sync_fd = sync_fd;
    win->events_fd = events_fd;
    win->window_id = wid;
    win->width = req->width;
    win->height = req->height;
    win->pitch = pitch;

    int32_t cx = 60 + g_cascade_offset * 32;
    int32_t cy = 48 + g_cascade_offset * 32;
    g_cascade_offset = (g_cascade_offset + 1) % 10;
    win->x = cx;
    win->y = cy;

    memset(win->title, 0, sizeof(win->title));
    if (req->title_length > 0) {
        size_t tlen = req->title_length;
        if (tlen > sizeof(win->title) - 1) {
            tlen = sizeof(win->title) - 1;
        }
        memcpy(win->title, req->title, tlen);
    }

    strncpy(win->surface_path, surface_path, sizeof(win->surface_path) - 1);
    win->surface_path[sizeof(win->surface_path) - 1] = '\0';
    strncpy(win->sync_path, sync_path, sizeof(win->sync_path) - 1);
    win->sync_path[sizeof(win->sync_path) - 1] = '\0';
    strncpy(win->events_path, events_path, sizeof(win->events_path) - 1);
    win->events_path[sizeof(win->events_path) - 1] = '\0';

    stlxgfx_create_window_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.window_id = wid;
    strncpy(resp.surface_name, surface_name, sizeof(resp.surface_name) - 1);
    strncpy(resp.sync_name, sync_name, sizeof(resp.sync_name) - 1);
    strncpy(resp.events_name, events_name, sizeof(resp.events_name) - 1);
    resp.width = req->width;
    resp.height = req->height;
    resp.pitch = pitch;
    resp.bpp = fb->bpp;
    resp.red_shift = fb->red_shift;
    resp.green_shift = fb->green_shift;
    resp.blue_shift = fb->blue_shift;
    resp.result_code = 0;

    if (send_message(client_fd, STLXGFX_MSG_CREATE_WINDOW_RESP,
                     req_header->sequence_number,
                     &resp, sizeof(resp)) != 0) {
        stlxgfx_dm_destroy_window(win);
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
    if (window->event_ring) {
        munmap(window->event_ring, sizeof(stlxgfx_event_ring_t));
    }
    if (window->sync) {
        munmap(window->sync, sizeof(stlxgfx_window_sync_t));
    }
    if (window->surface_buf) {
        munmap(window->surface_buf, window->surface_size);
    }
    if (window->events_fd >= 0) {
        close(window->events_fd);
    }
    if (window->surface_fd >= 0) {
        close(window->surface_fd);
    }
    if (window->sync_fd >= 0) {
        close(window->sync_fd);
    }
    if (window->events_path[0]) {
        unlink(window->events_path);
    }
    if (window->surface_path[0]) {
        unlink(window->surface_path);
    }
    if (window->sync_path[0]) {
        unlink(window->sync_path);
    }
    free(window);
}

int stlxgfx_dm_sync(stlxgfx_dm_window_t* window) {
    if (!window || !window->sync) {
        return 0;
    }
    stlxgfx_window_sync_t* s = window->sync;

    int new_frame = 0;
    uint32_t ready = atomic_load_explicit(&s->frame_ready, memory_order_acquire);
    uint32_t pending = atomic_load_explicit(&s->swap_pending, memory_order_acquire);

    if (ready && pending) {
        uint32_t ri = atomic_load_explicit(&s->ready_index, memory_order_acquire);
        atomic_store_explicit(&s->front_index, ri, memory_order_relaxed);
        atomic_store_explicit(&s->frame_ready, 0, memory_order_relaxed);
        atomic_store_explicit(&s->swap_pending, 0, memory_order_release);

        size_t single_buf = (size_t)window->pitch * window->height;
        window->front->pixels = window->surface_buf + ri * single_buf;
        new_frame = 1;
    }

    atomic_store_explicit(&s->dm_consuming, 1, memory_order_release);
    return new_frame;
}

void stlxgfx_dm_finish_sync(stlxgfx_dm_window_t* window) {
    if (!window || !window->sync) {
        return;
    }
    atomic_store_explicit(&window->sync->dm_consuming, 0, memory_order_release);
}

stlxgfx_surface_t* stlxgfx_dm_front_buffer(stlxgfx_dm_window_t* window) {
    if (!window) {
        return NULL;
    }
    return window->front;
}


