#ifndef STELLUX_ARCH_AARCH64_SYSCALL_EPOLL_EVENT_H
#define STELLUX_ARCH_AARCH64_SYSCALL_EPOLL_EVENT_H

#include "common/types.h"

namespace syscall {

struct epoll_event {
    uint32_t events;
    uint64_t data;
};

static_assert(sizeof(epoll_event) == 16, "aarch64 pads epoll_event");

} // namespace syscall

#endif // STELLUX_ARCH_AARCH64_SYSCALL_EPOLL_EVENT_H
