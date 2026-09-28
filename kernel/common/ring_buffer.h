#ifndef STELLUX_COMMON_RING_BUFFER_H
#define STELLUX_COMMON_RING_BUFFER_H

#include "common/types.h"
#include "common/list.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

constexpr size_t RING_BUFFER_DEFAULT_CAPACITY = 8192;

// Values mirror the resource layer error codes and flow untranslated
constexpr ssize_t RB_ERR_INVAL = -1;
constexpr ssize_t RB_ERR_AGAIN = -16;
constexpr ssize_t RB_ERR_PIPE  = -11;
constexpr ssize_t RB_ERR_INTR  = -18;

// A stretch of written bytes carrying something for the read that reaches it
struct ring_buffer_mark {
    list::node link;
    size_t position;
    size_t length;
};

using ring_buffer_mark_list = list::head<ring_buffer_mark, &ring_buffer_mark::link>;
using ring_buffer_mark_fn = void (*)(ring_buffer_mark* mark, void* context);

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
    ring_buffer_mark_list marks;
};

/**
 * Allocate and initialize a ring buffer.
 * Control struct from privileged heap, data from unprivileged heap.
 * @return Ring buffer pointer on success, nullptr on allocation failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ring_buffer* ring_buffer_create(size_t capacity);

/**
 * Free a ring buffer and its data. Must only be called when no waiters and no marks remain.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_destroy(ring_buffer* rb);

/**
 * Read from ring buffer. Blocks when empty unless nonblock is true.
 * @return Bytes read (> 0), 0 on EOF once drained after either side closed,
 *   RB_ERR_AGAIN if nonblock and empty, or negative error.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_read(ring_buffer* rb, uint8_t* buf, size_t len, bool nonblock = false);

/**
 * Wait until the buffer holds data or either side has closed, without reading.
 * @return Readable bytes, 0 at end of stream, RB_ERR_AGAIN when nonblock finds
 *   nothing, or RB_ERR_INTR when a signal interrupted the wait.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_wait_readable(ring_buffer* rb, bool nonblock);

/**
 * Copy up to `len` queued bytes without consuming them. Never blocks.
 * @return Bytes copied.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE size_t ring_buffer_peek(ring_buffer* rb, uint8_t* buf, size_t len);

/**
 * Consume up to `len` queued bytes without copying them. Never blocks.
 * @return Bytes consumed.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE size_t ring_buffer_skip(ring_buffer* rb, size_t len);

/**
 * Take up to `len` queued bytes without blocking, stopping where a marked stretch ends, and copy them
 * into `buf` unless it is null. The call consuming a marked stretch's first byte takes its mark into
 * `taken`. A buffer holding marks must be consumed only through this function.
 * @return Bytes taken (> 0), 0 at end of stream, or RB_ERR_AGAIN when nothing is queued.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_read_marked(ring_buffer* rb, uint8_t* buf, size_t len,
                                                                ring_buffer_mark** taken);

/**
 * Look at queued bytes like ring_buffer_read_marked, without consuming them. When they reach a marked
 * stretch, `on_mark`, if given, runs with its mark under the buffer's lock, so it must not sleep.
 * @return Bytes seen (> 0), 0 at end of stream, or RB_ERR_AGAIN when nothing is queued.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_peek_marked(ring_buffer* rb, uint8_t* buf, size_t len,
                                                                ring_buffer_mark_fn on_mark, void* context);

/**
 * Write to ring buffer. Blocks when full unless nonblock is true.
 * @return Bytes written (> 0), RB_ERR_AGAIN if nonblock and full, RB_ERR_PIPE if either side closed.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_write(ring_buffer* rb, const uint8_t* buf, size_t len, bool nonblock = false);

/**
 * Write like ring_buffer_write, attaching `mark`, when given, to exactly the bytes this call writes.
 * A call that writes nothing leaves the mark untouched.
 * @return Bytes written (> 0), RB_ERR_AGAIN if nonblock and full, RB_ERR_PIPE if either side closed,
 *   or RB_ERR_INTR when a signal interrupted the wait.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_write_marked(ring_buffer* rb, const uint8_t* buf, size_t len,
                                                                 ring_buffer_mark* mark, bool nonblock);

/**
 * All-or-nothing write: writes all `len` bytes atomically or none.
 * In nonblock mode returns RB_ERR_AGAIN when insufficient space,
 * in blocking mode waits until enough space is available.
 * @return len on success, or negative error.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE ssize_t ring_buffer_write_all(ring_buffer* rb, const uint8_t* buf, size_t len, bool nonblock = false);

/**
 * Mark the write side as closed. Wakes all blocked readers and writers, so readers
 * see EOF once drained and writers get RB_ERR_PIPE.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_close_write(ring_buffer* rb);

/**
 * Mark the read side as closed. Wakes all blocked readers and writers, so readers
 * see EOF once drained and writers get RB_ERR_PIPE.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_close_read(ring_buffer* rb);

/**
 * Move every mark into `marks`, for an owner releasing what no read will take.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_take_marks(ring_buffer* rb, ring_buffer_mark_list& marks);

namespace sync { struct poll_table; }

/**
 * Check read-direction readiness and optionally subscribe for wakeup.
 * @return Bitmask: POLL_IN if data available, POLL_HUP once either side has closed.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t ring_buffer_poll_read(ring_buffer* rb, sync::poll_table* pt);

/**
 * Check write-direction readiness and optionally subscribe for wakeup.
 * @return Bitmask: POLL_OUT if space available, POLL_ERR once either side has closed.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t ring_buffer_poll_write(ring_buffer* rb, sync::poll_table* pt);

#endif // STELLUX_COMMON_RING_BUFFER_H
