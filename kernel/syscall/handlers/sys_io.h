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

/**
 * @brief Copies the user buffers `iovs` describe into `data`, which must hold their total length.
 * @return 0, or EFAULT when a buffer cannot be read.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t gather_from_user(const iovec* iovs, uint64_t iovcnt, uint8_t* data);

/**
 * @brief Copies `len` bytes of `data` across the user buffers `iovs` describe, in order.
 * @return 0, or EFAULT when a buffer cannot be written.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t scatter_to_user(const iovec* iovs, uint64_t iovcnt, const uint8_t* data, size_t len);

/**
 * @brief The largest message `fd` carries when it is a message socket, whose reads and writes each
 * move exactly one whole message, or 0 for anything else.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE size_t message_limit(sched::task* task, resource::handle_t fd);

/**
 * @brief Reads one message from the message socket `fd` into the user buffers `iovs` describe,
 * keeping what fits and discarding the rest. `max_message` is the socket's largest message.
 * @return Bytes copied, or a negative errno.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t read_message(sched::task* task, resource::handle_t fd, const iovec* iovs,
                                       uint64_t iovcnt, size_t max_message, uint32_t call_flags);

/**
 * @brief Writes the user buffers `iovs` describe to the message socket `fd` as one message.
 * @return Bytes written, EMSGSIZE when they hold more than `max_message` bytes, or a negative errno.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t write_message(sched::task* task, resource::handle_t fd, const iovec* iovs,
                                        uint64_t iovcnt, size_t max_message, uint32_t call_flags);

} // namespace syscall

DECLARE_SYSCALL(readv);
DECLARE_SYSCALL(writev);
DECLARE_SYSCALL(ioctl);

#endif // STELLUX_SYSCALL_HANDLERS_SYS_IO_H
