#ifndef STELLUX_RESOURCE_RESOURCE_TYPES_H
#define STELLUX_RESOURCE_RESOURCE_TYPES_H

#include "common/types.h"

namespace resource {

enum class resource_type : uint16_t {
    UNKNOWN  = 0,
    FILE     = 1,
    SOCKET   = 2,
    SHMEM    = 3,
    PROCESS  = 4,
    TERMINAL = 5,
    PTY      = 6,
    PIPE     = 7,
};

using handle_t = int32_t;

constexpr uint32_t RIGHT_READ  = (1u << 0);
constexpr uint32_t RIGHT_WRITE = (1u << 1);
constexpr uint32_t RIGHT_MASK  = RIGHT_READ | RIGHT_WRITE;

} // namespace resource

#endif // STELLUX_RESOURCE_RESOURCE_TYPES_H
