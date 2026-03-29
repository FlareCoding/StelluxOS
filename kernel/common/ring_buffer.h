#ifndef STELLUX_COMMON_RING_BUFFER_H
#define STELLUX_COMMON_RING_BUFFER_H

#include "common/types.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

constexpr size_t RING_BUFFER_DEFAULT_CAPACITY = 8192;

constexpr ssize_t RB_ERR_INVAL = -1;
constexpr ssize_t RB_ERR_AGAIN = -16;
constexpr ssize_t RB_ERR_PIPE  = -11;

struct ring_buffer {
    uint8_t* data;
    size_t capacity;
    size_t head; // write position
    size_t tail; // read position
    bool writer_closed;
    bool reader_closed;
    sync::spinlock lock;
    sync::wait_queue read_wq;
    sync::wait_queue write_wq;
};

/**
 * Allocate and initialize a ring buffer.
 * Control struct from privileged heap, data from unprivileged heap.
 * @return Ring buffer pointer on success, nullptr on allocation failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ring_buffer* ring_buffer_create(size_t capacity);

/**
 * Free a ring buffer and its data. Must only be called when no waiters remain.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_destroy(ring_buffer* rb);

/**
 * Read from ring buffer. Blocks when empty unless nonblock is true.
 * @return Bytes read (> 0), 0 on EOF, RB_ERR_AGAIN if nonblock and empty, or negative error.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_read(ring_buffer* rb, uint8_t* buf, size_t len, bool nonblock = false);

/**
 * Write to ring buffer. Blocks when full unless nonblock is true.
 * @return Bytes written (> 0), RB_ERR_AGAIN if nonblock and full, RB_ERR_PIPE if reader closed.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_write(ring_buffer* rb, const uint8_t* buf, size_t len, bool nonblock = false);

/**
 * All-or-nothing write: writes all `len` bytes atomically or none.
 * In nonblock mode returns RB_ERR_AGAIN when insufficient space;
 * in blocking mode waits until enough space is available.
 * @return len on success, or negative error.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_write_all(ring_buffer* rb, const uint8_t* buf, size_t len, bool nonblock = false);

/**
 * Mark the write side as closed. Wakes all blocked readers so they can see EOF.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_close_write(ring_buffer* rb);

/**
 * Mark the read side as closed. Wakes all blocked writers so they get RB_ERR_PIPE.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_close_read(ring_buffer* rb);

namespace sync { struct poll_table; struct poll_entry; }

/**
 * Check read-direction readiness and optionally subscribe for wakeup.
 * @return Bitmask: POLL_IN if data available, POLL_HUP if writer closed and empty.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t ring_buffer_poll_read(ring_buffer* rb, sync::poll_table* pt, sync::poll_entry* entry);

/**
 * Check write-direction readiness and optionally subscribe for wakeup.
 * @return Bitmask: POLL_OUT if space available, POLL_ERR if reader closed.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t ring_buffer_poll_write(ring_buffer* rb, sync::poll_table* pt, sync::poll_entry* entry);

#endif // STELLUX_COMMON_RING_BUFFER_H
