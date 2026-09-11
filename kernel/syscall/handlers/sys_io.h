#ifndef STELLUX_SYSCALL_HANDLERS_SYS_IO_H
#define STELLUX_SYSCALL_HANDLERS_SYS_IO_H

#include "syscall/syscall_table.h"

namespace syscall {

struct iovec {
    uint64_t base;
    uint64_t len;
};

constexpr uint64_t MAX_IOVCNT = 1024;

} // namespace syscall

DECLARE_SYSCALL(readv);
DECLARE_SYSCALL(writev);
DECLARE_SYSCALL(ioctl);

#endif // STELLUX_SYSCALL_HANDLERS_SYS_IO_H
