#ifndef COMMANDS_H
#define COMMANDS_H
#include <types.h>

#define STELLA_COMMAND_ID_CREATE_SESSION    0x100
#define STELLA_COMMAND_ID_CREATE_WINDOW     0x800
#define STELLA_COMMAND_ID_MAP_CANVAS        0x900

#define STELLA_RESPONSE_ID_MAP_FRAMEBUFFER  0x400

namespace stella_ui::internal {
struct userlib_request_header {
    uint64_t type;
    uint64_t session_id;
} __attribute__((packed));

struct userlib_request_create_session {
    userlib_request_header header;
    char name[128];
};

struct userlib_request_create_window {
    userlib_request_header header;
    uint32_t width;
    uint32_t height;
    uint32_t bg_color;
    char title[128];
};

struct userlib_response_header {
    uint64_t type;
} __attribute__((packed));

struct userlib_response_map_window_framebuffer {
    userlib_response_header header;
    uint32_t    width;
    uint32_t    height;
    uint32_t    pitch;
    uint8_t     bpp;
    uintptr_t   physical_page_ptr;
    uintptr_t   page_offset;
    size_t      page_count;
};
} // namespace stella_ui::internal

#endif // COMMANDS_H

