#ifndef STLXGFX_PROTOCOL_H
#define STLXGFX_PROTOCOL_H

#include <stdint.h>

// Protocol configuration
#define STLXGFX_PROTOCOL_VERSION 0x000001
#define STLXGFX_DM_SOCKET_PATH "/tmp/stlxdm.socket"
#define STLXGFX_MAX_PAYLOAD_SIZE 4096

// Message types
typedef enum {
    STLXGFX_MSG_CREATE_WINDOW_REQUEST   = 0x0001,
    STLXGFX_MSG_CREATE_WINDOW_RESPONSE  = 0x0002,
    STLXGFX_MSG_DESTROY_WINDOW_REQUEST  = 0x0003,
    STLXGFX_MSG_DESTROY_WINDOW_RESPONSE = 0x0004,
    STLXGFX_MSG_ERROR_RESPONSE          = 0xFFFF
} stlxgfx_message_type_t;

// Message header (fixed size: 20 bytes)
typedef struct {
    uint32_t protocol_version;
    uint32_t message_type;
    uint32_t sequence_number;
    uint32_t payload_size;
    uint32_t flags;
} __attribute__((packed)) stlxgfx_message_header_t;

// Create window request payload
typedef struct {
    uint32_t width;
    uint32_t height;
    int32_t posx;
    int32_t posy;
    uint32_t title_length;
    char title[256];
} __attribute__((packed)) stlxgfx_create_window_request_t;

// Create window response payload
typedef struct {
    uint32_t window_id;
    uint64_t sync_shm_handle;     // Window sync shared memory handle
    uint64_t surface_shm_handle;  // Surface pair shared memory handle
    uint32_t surface_format;
    uint32_t result_code;         // 0 = success, negative = error
} __attribute__((packed)) stlxgfx_create_window_response_t;

// Error response payload
typedef struct {
    uint32_t error_code;
    uint32_t original_sequence;
    char error_message[128];
} __attribute__((packed)) stlxgfx_error_response_t;

// Window synchronization structure (lives in shared memory)
typedef struct stlxgfx_window_sync {
    // Triple-buffering synchronization
    volatile uint32_t front_buffer_index;   // Which buffer DM is reading from (0,1,2)
    volatile uint32_t back_buffer_index;    // Which buffer app is drawing to (0,1,2)
    volatile uint32_t ready_buffer_index;   // Which buffer is ready to become front (0,1,2)
    
    volatile uint32_t frame_ready;          // New frame available for swap (0/1)
    volatile uint32_t dm_consuming;         // DM is currently reading front buffer (0/1)
    volatile uint32_t swap_pending;         // Swap operation pending (0/1)
    
    // Window state (read-only for app, managed by DM)
    volatile uint32_t window_visible;       // Window visibility state
    volatile uint32_t window_focused;       // Window focus state
    volatile uint32_t close_requested;      // DM requests window close
    volatile uint32_t reserved;             // Reserved for future use
    
    // Padding for cache line alignment (64 bytes total)
    uint32_t padding[6];
} stlxgfx_window_sync_t;

// Error codes
#define STLXGFX_ERROR_SUCCESS           0
#define STLXGFX_ERROR_INVALID_SIZE     -1
#define STLXGFX_ERROR_OUT_OF_MEMORY    -2
#define STLXGFX_ERROR_PROTOCOL_ERROR   -3
#define STLXGFX_ERROR_INTERNAL_ERROR   -4

#endif // STLXGFX_PROTOCOL_H
 