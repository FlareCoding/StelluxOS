#ifndef STLXGFX_WINDOW_H
#define STLXGFX_WINDOW_H

#include <stlxgfx/fb.h>
#include <stlxgfx/event.h>
#include <stdatomic.h>

/* --- Socket path --- */

#define STLXGFX_DM_SOCKET_PATH "/tmp/stlxgfxdm.socket"

/* --- Protocol --- */

#define STLXGFX_PROTOCOL_VERSION 1

#define STLXGFX_MSG_CREATE_WINDOW_REQ   0x0001
#define STLXGFX_MSG_CREATE_WINDOW_RESP  0x0002
#define STLXGFX_MSG_DESTROY_WINDOW_REQ  0x0003
#define STLXGFX_MSG_DESTROY_WINDOW_RESP 0x0004
#define STLXGFX_MSG_ERROR_RESP          0xFFFF

typedef struct {
    uint32_t protocol_version;
    uint32_t message_type;
    uint32_t sequence_number;
    uint32_t payload_size;
    uint32_t flags;
} __attribute__((packed)) stlxgfx_msg_header_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t title_length;
    char     title[256];
} __attribute__((packed)) stlxgfx_create_window_req_t;

typedef struct {
    uint32_t window_id;
    char     surface_name[64];
    char     sync_name[64];
    char     events_name[64];
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    uint8_t  padding;
    uint32_t result_code;
} __attribute__((packed)) stlxgfx_create_window_resp_t;

typedef struct {
    uint32_t window_id;
} __attribute__((packed)) stlxgfx_destroy_window_req_t;

typedef struct {
    uint32_t window_id;
    uint32_t result_code;
} __attribute__((packed)) stlxgfx_destroy_window_resp_t;

typedef struct {
    uint32_t error_code;
    uint32_t original_sequence;
    char     error_message[128];
} __attribute__((packed)) stlxgfx_error_resp_t;

/* --- Triple-buffer sync (shared memory) --- */

typedef struct {
    atomic_uint_least32_t front_index;
    atomic_uint_least32_t back_index;
    atomic_uint_least32_t ready_index;
    atomic_uint_least32_t frame_ready;
    atomic_uint_least32_t dm_consuming;
    atomic_uint_least32_t swap_pending;
    atomic_uint_least32_t close_requested;
} stlxgfx_window_sync_t;

/* --- Client-side API --- */

typedef struct stlxgfx_window_t_tag {
    stlxgfx_window_sync_t* sync;
    stlxgfx_event_ring_t*  event_ring;
    uint8_t* surface_buf;
    size_t   surface_size;
    int      surface_fd;
    int      sync_fd;
    int      events_fd;
    uint32_t window_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    stlxgfx_surface_t* back;
    int      conn_fd;
} stlxgfx_window_t;

int stlxgfx_connect(const char* socket_path);
void stlxgfx_disconnect(int conn_fd);

stlxgfx_window_t* stlxgfx_create_window(int conn_fd, uint32_t width,
                                          uint32_t height, const char* title);
void stlxgfx_window_close(stlxgfx_window_t* window);
stlxgfx_surface_t* stlxgfx_window_back_buffer(stlxgfx_window_t* window);
int stlxgfx_window_swap_buffers(stlxgfx_window_t* window);
int stlxgfx_window_should_close(stlxgfx_window_t* window);

/* --- DM-side API --- */

#define STLXGFX_DM_MAX_CLIENTS 16

typedef struct {
    stlxgfx_window_sync_t* sync;
    stlxgfx_event_ring_t*  event_ring;
    uint8_t* surface_buf;
    size_t   surface_size;
    int      surface_fd;
    int      sync_fd;
    int      events_fd;
    uint32_t window_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    stlxgfx_surface_t* front;
    int32_t  x, y;
    char     title[256];
    char     surface_path[128];
    char     sync_path[128];
    char     events_path[128];
} stlxgfx_dm_window_t;

int stlxgfx_dm_listen(const char* socket_path);
int stlxgfx_dm_accept(int listen_fd);
int stlxgfx_dm_read_request(int client_fd, stlxgfx_msg_header_t* header,
                             void* payload, size_t max_payload);
stlxgfx_dm_window_t* stlxgfx_dm_handle_create_window(
    int client_fd, const stlxgfx_msg_header_t* req_header,
    const stlxgfx_create_window_req_t* req, const stlxgfx_fb_t* fb);
void stlxgfx_dm_destroy_window(stlxgfx_dm_window_t* window);
int stlxgfx_dm_sync(stlxgfx_dm_window_t* window);
void stlxgfx_dm_finish_sync(stlxgfx_dm_window_t* window);
stlxgfx_surface_t* stlxgfx_dm_front_buffer(stlxgfx_dm_window_t* window);

#endif /* STLXGFX_WINDOW_H */
