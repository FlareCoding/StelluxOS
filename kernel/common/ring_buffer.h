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
 * Return the number of bytes that can be written without blocking.
 * Caller must hold rb->lock or be in a context where head/tail are stable.
 */
static inline size_t ring_buffer_writable(const ring_buffer* rb) {
    size_t used = (rb->head - rb->tail) & (rb->capacity - 1);
    return rb->capacity - 1 - used;
}

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
 * Mark the write side as closed. Wakes all blocked readers so they can see EOF.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_close_write(ring_buffer* rb);

/**
 * Mark the read side as closed. Wakes all blocked writers so they get RB_ERR_PIPE.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_close_read(ring_buffer* rb);

#endif // STELLUX_COMMON_RING_BUFFER_H
