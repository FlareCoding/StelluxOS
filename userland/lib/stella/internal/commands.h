#ifndef COMMANDS_H
#define COMMANDS_H
#include <types.h>

#define STELLA_COMMAND_ID_CREATE_SESSION    0x100
#define STELLA_COMMAND_ID_CREATE_WINDOW     0x800

namespace stella_ui::internal {
struct userlib_request_header {
    uint64_t type;
} __attribute__((packed));

struct userlib_request_create_session {
    userlib_request_header header;
    char name[128];
};

struct userlib_request_create_window {
    userlib_request_header header;
    uint32_t width;
    uint32_t height;
    char title[128];
};
} // namespace stella_ui::internal

#endif // COMMANDS_H

