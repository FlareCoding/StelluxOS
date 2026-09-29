#ifndef STELLUX_ARCH_X86_64_SYSCALL_EPOLL_EVENT_H
#define STELLUX_ARCH_X86_64_SYSCALL_EPOLL_EVENT_H

#include "common/types.h"

namespace syscall {

// The x86_64 ABI packs the record to 12 bytes
struct __attribute__((packed)) epoll_event {
    uint32_t events;
    uint64_t data;
};

static_assert(sizeof(epoll_event) == 12, "x86_64 packs epoll_event");

} // namespace syscall

#endif // STELLUX_ARCH_X86_64_SYSCALL_EPOLL_EVENT_H
