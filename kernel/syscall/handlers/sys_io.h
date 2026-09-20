#ifndef STELLUX_SYSCALL_HANDLERS_SYS_IO_H
#define STELLUX_SYSCALL_HANDLERS_SYS_IO_H

#include "syscall/syscall_table.h"
#include "resource/resource.h"

namespace sched { struct task; }

namespace syscall {

struct iovec {
    uint64_t base;
    uint64_t len;
};

constexpr uint64_t MAX_IOVCNT        = 1024;
constexpr size_t   IO_CHUNK_SIZE     = 4096;
constexpr size_t   STREAM_CHUNK_SIZE = 16384;

/**
 * @brief Bytes a read or write stages per round through `fd`: STREAM_CHUNK_SIZE
 * for a stream socket, IO_CHUNK_SIZE for anything else.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE size_t io_chunk_size(sched::task* task, resource::handle_t fd);

} // namespace syscall

DECLARE_SYSCALL(readv);
DECLARE_SYSCALL(writev);
DECLARE_SYSCALL(ioctl);

#endif // STELLUX_SYSCALL_HANDLERS_SYS_IO_H
