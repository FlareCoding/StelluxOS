#include "common/ring_buffer.h"
#include "sync/poll.h"
#include "sync/wait_queue.h"
#include "mm/heap.h"
#include "common/string.h"
#include "sched/sched.h"
#include "signals/signal.h"

// Wakes go to every waiter, since one that only peeks, or takes part of what it
// waited for, leaves the rest to the others

static inline size_t readable_bytes(const ring_buffer* rb) {
    return (rb->head - rb->tail) & (rb->capacity - 1);
}

static inline size_t writable_bytes(const ring_buffer* rb) {
    return rb->capacity - 1 - readable_bytes(rb);
}

// Whether `len` more bytes fit, along with a mark when the write carries one
static inline bool has_room(const ring_buffer* rb, size_t len, bool marked) {
    return writable_bytes(rb) >= len && (!marked || rb->marks.size() < rb->max_marks);
}

// Once either side closes, a drained buffer reads as the end of the stream and refuses writes
static inline bool is_shut(const ring_buffer* rb) {
    return rb->writer_closed || rb->reader_closed;
}

// Copies `len` bytes from the read position without consuming them. The caller must
// hold the lock and know that `len` bytes are queued.
static void copy_queued(const ring_buffer* rb, uint8_t* buf, size_t len) {
    size_t tail_idx = rb->tail & (rb->capacity - 1);
    size_t first = rb->capacity - tail_idx;
    if (first > len) {
        first = len;
    }

    string::memcpy(buf, rb->data + tail_idx, first);
    if (first < len) {
        string::memcpy(buf + first, rb->data, len - first);
    }
}

/**
 * Wakes whoever polls the reading side. The caller holds the lock, which keeps the queue from being
 * cleared and freed mid-wake.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void wake_reader_pollers_locked(ring_buffer* rb) {
    if (rb->reader_poll_wq) {
        sync::wake_all(*rb->reader_poll_wq);
    }
}

/**
 * Wakes whoever polls the writing side, under the lock as for readers.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void wake_writer_pollers_locked(ring_buffer* rb) {
    if (rb->writer_poll_wq) {
        sync::wake_all(*rb->writer_poll_wq);
    }
}

/**
 * How many of `len` queued bytes one marked read or peek covers, stopping where a marked stretch ends,
 * with the mark whose stretch it reaches in `reached`. The caller must hold the lock.
 * @return The byte count (> 0), 0 at end of stream, or RB_ERR_AGAIN when nothing is queued.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t marked_read_span(ring_buffer* rb, size_t len, ring_buffer_mark** reached) {
    size_t avail = readable_bytes(rb);
    if (avail == 0) {
        return is_shut(rb) ? 0 : RB_ERR_AGAIN;
    }

    size_t span = avail < len ? avail : len;

    ring_buffer_mark* mark = rb->marks.front();
    if (mark && mark->position < rb->tail + span) {
        size_t to_stretch_end = mark->position + mark->length - rb->tail;
        span = to_stretch_end < span ? to_stretch_end : span;
        *reached = mark;
    }

    return static_cast<ssize_t>(span);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ring_buffer* ring_buffer_create(size_t capacity, size_t max_marks) {
    if (capacity == 0) {
        return nullptr;
    }

    size_t cap = 1;
    while (cap < capacity + 1) {
        cap <<= 1;
    }

    auto* rb = static_cast<ring_buffer*>(heap::kzalloc(sizeof(ring_buffer)));
    if (!rb) {
        return nullptr;
    }

    rb->data = static_cast<uint8_t*>(heap::uzalloc(cap));
    if (!rb->data) {
        heap::kfree(rb);
        return nullptr;
    }

    rb->capacity = cap;
    rb->head = 0;
    rb->tail = 0;
    rb->writer_closed = false;
    rb->reader_closed = false;
    rb->lock = sync::SPINLOCK_INIT;
    rb->read_wq.init();
    rb->write_wq.init();
    rb->marks.init();
    rb->max_marks = max_marks;
    rb->reader_poll_wq = nullptr;
    rb->writer_poll_wq = nullptr;

    return rb;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_destroy(ring_buffer* rb) {
    if (!rb) {
        return;
    }

    if (rb->data) {
        heap::ufree(rb->data);
        rb->data = nullptr;
    }
    heap::kfree(rb);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_set_reader_poll_queue(ring_buffer* rb, sync::wait_queue* wq) {
    sync::irq_lock_guard guard(rb->lock);
    rb->reader_poll_wq = wq;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_set_writer_poll_queue(ring_buffer* rb, sync::wait_queue* wq) {
    sync::irq_lock_guard guard(rb->lock);
    rb->writer_poll_wq = wq;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t ring_buffer_read(ring_buffer* rb, uint8_t* buf, size_t len, bool nonblock) {
    if (!rb || !buf || len == 0) {
        return RB_ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);

    if (readable_bytes(rb) == 0 && !is_shut(rb)) {
        if (nonblock) {
            sync::spin_unlock_irqrestore(rb->lock, irq);
            return RB_ERR_AGAIN;
        }
        while (readable_bytes(rb) == 0 && !is_shut(rb) && !signals::interrupt_pending(sched::current())) {
            irq = sync::wait(rb->read_wq, rb->lock, irq);
        }
    }

    size_t avail = readable_bytes(rb);
    if (avail == 0) {
        // A shut buffer is genuine EOF, an interrupted wait is not
        bool intr = !is_shut(rb)
                  && signals::interrupt_pending(sched::current());
        sync::spin_unlock_irqrestore(rb->lock, irq);
        return intr ? RB_ERR_INTR : 0;
    }

    size_t to_read = avail < len ? avail : len;
    copy_queued(rb, buf, to_read);
    rb->tail += to_read;

    wake_writer_pollers_locked(rb);
    sync::spin_unlock_irqrestore(rb->lock, irq);
    sync::wake_all(rb->write_wq);

    return static_cast<ssize_t>(to_read);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t ring_buffer_wait_readable(ring_buffer* rb, bool nonblock) {
    if (!rb) {
        return RB_ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);

    ssize_t result = 0;
    while (readable_bytes(rb) == 0 && !is_shut(rb)) {
        if (nonblock) {
            result = RB_ERR_AGAIN;
            break;
        }

        if (signals::interrupt_pending(sched::current())) {
            result = RB_ERR_INTR;
            break;
        }

        irq = sync::wait(rb->read_wq, rb->lock, irq);
    }

    if (result == 0) {
        result = static_cast<ssize_t>(readable_bytes(rb));
    }

    sync::spin_unlock_irqrestore(rb->lock, irq);

    return result;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE size_t ring_buffer_peek(ring_buffer* rb, uint8_t* buf, size_t len) {
    if (!rb || !buf) {
        return 0;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);
    size_t avail = readable_bytes(rb);
    size_t to_copy = avail < len ? avail : len;
    copy_queued(rb, buf, to_copy);
    sync::spin_unlock_irqrestore(rb->lock, irq);

    return to_copy;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE size_t ring_buffer_skip(ring_buffer* rb, size_t len) {
    if (!rb) {
        return 0;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);
    size_t avail = readable_bytes(rb);
    size_t to_skip = avail < len ? avail : len;
    rb->tail += to_skip;

    if (to_skip > 0) {
        wake_writer_pollers_locked(rb);
    }

    sync::spin_unlock_irqrestore(rb->lock, irq);

    if (to_skip > 0) {
        sync::wake_all(rb->write_wq);
    }

    return to_skip;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t ring_buffer_read_marked(ring_buffer* rb, uint8_t* buf, size_t len,
                                                  ring_buffer_mark** taken) {
    if (!rb || len == 0 || !taken) {
        return RB_ERR_INVAL;
    }

    *taken = nullptr;

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);

    ring_buffer_mark* reached = nullptr;
    ssize_t span = marked_read_span(rb, len, &reached);
    if (span <= 0) {
        sync::spin_unlock_irqrestore(rb->lock, irq);
        return span;
    }

    if (buf) {
        copy_queued(rb, buf, static_cast<size_t>(span));
    }

    if (reached) {
        rb->marks.remove(reached);
        *taken = reached;
    }

    rb->tail += static_cast<size_t>(span);

    wake_writer_pollers_locked(rb);
    sync::spin_unlock_irqrestore(rb->lock, irq);
    sync::wake_all(rb->write_wq);

    return span;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t ring_buffer_peek_marked(ring_buffer* rb, uint8_t* buf, size_t len,
                                                  ring_buffer_mark_fn on_mark, void* context) {
    if (!rb || len == 0) {
        return RB_ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);

    ring_buffer_mark* reached = nullptr;
    ssize_t span = marked_read_span(rb, len, &reached);

    if (span > 0 && buf) {
        copy_queued(rb, buf, static_cast<size_t>(span));
    }

    if (reached && on_mark) {
        on_mark(reached, context);
    }

    sync::spin_unlock_irqrestore(rb->lock, irq);

    return span;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t ring_buffer_write_marked(ring_buffer* rb, const uint8_t* buf, size_t len,
                                                   ring_buffer_mark* mark, bool nonblock) {
    if (!rb || !buf || len == 0) {
        return RB_ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);

    bool marked = mark != nullptr;
    if (!has_room(rb, 1, marked) && !is_shut(rb)) {
        if (nonblock) {
            sync::spin_unlock_irqrestore(rb->lock, irq);
            return RB_ERR_AGAIN;
        }
        while (!has_room(rb, 1, marked) && !is_shut(rb) && !signals::interrupt_pending(sched::current())) {
            irq = sync::wait(rb->write_wq, rb->lock, irq);
        }
    }

    if (is_shut(rb)) {
        sync::spin_unlock_irqrestore(rb->lock, irq);
        return RB_ERR_PIPE;
    }

    // Progress wins over interruption: only a waited-out interruption
    // leaves no room here while neither side is closed
    if (!has_room(rb, 1, marked)) {
        sync::spin_unlock_irqrestore(rb->lock, irq);
        return RB_ERR_INTR;
    }

    size_t space = writable_bytes(rb);
    size_t to_write = space < len ? space : len;
    if (mark) {
        mark->position = rb->head;
        mark->length = to_write;
        rb->marks.push_back(mark);
    }

    size_t head_idx = rb->head & (rb->capacity - 1);
    size_t first = rb->capacity - head_idx;
    if (first > to_write) {
        first = to_write;
    }

    string::memcpy(rb->data + head_idx, buf, first);
    if (first < to_write) {
        string::memcpy(rb->data, buf + first, to_write - first);
    }

    rb->head += to_write;

    wake_reader_pollers_locked(rb);
    sync::spin_unlock_irqrestore(rb->lock, irq);
    sync::wake_all(rb->read_wq);

    return static_cast<ssize_t>(to_write);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t ring_buffer_write(ring_buffer* rb, const uint8_t* buf, size_t len, bool nonblock) {
    return ring_buffer_write_marked(rb, buf, len, nullptr, nonblock);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t ring_buffer_write_all(ring_buffer* rb, const uint8_t* buf, size_t len, bool nonblock) {
    return ring_buffer_write_all_marked(rb, buf, len, nullptr, nonblock);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t ring_buffer_write_all_marked(ring_buffer* rb, const uint8_t* buf, size_t len,
                                                       ring_buffer_mark* mark, bool nonblock) {
    if (!rb || !buf || len == 0) {
        return RB_ERR_INVAL;
    }

    // Reject writes that can never succeed (len exceeds max writable space)
    if (len > rb->capacity - 1) {
        return RB_ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);

    bool marked = mark != nullptr;
    if (!has_room(rb, len, marked) && !is_shut(rb)) {
        if (nonblock) {
            sync::spin_unlock_irqrestore(rb->lock, irq);
            return RB_ERR_AGAIN;
        }
        while (!has_room(rb, len, marked) && !is_shut(rb) && !signals::interrupt_pending(sched::current())) {
            irq = sync::wait(rb->write_wq, rb->lock, irq);
        }
    }

    if (is_shut(rb)) {
        sync::spin_unlock_irqrestore(rb->lock, irq);
        return RB_ERR_PIPE;
    }

    // Progress wins over interruption, AGAIN covers spurious exits
    if (!has_room(rb, len, marked)) {
        bool intr = signals::interrupt_pending(sched::current());
        sync::spin_unlock_irqrestore(rb->lock, irq);
        return intr ? RB_ERR_INTR : RB_ERR_AGAIN;
    }

    if (mark) {
        mark->position = rb->head;
        mark->length = len;
        rb->marks.push_back(mark);
    }

    size_t head_idx = rb->head & (rb->capacity - 1);
    size_t first = rb->capacity - head_idx;
    if (first > len) {
        first = len;
    }

    string::memcpy(rb->data + head_idx, buf, first);
    if (first < len) {
        string::memcpy(rb->data, buf + first, len - first);
    }

    rb->head += len;

    wake_reader_pollers_locked(rb);
    sync::spin_unlock_irqrestore(rb->lock, irq);
    sync::wake_all(rb->read_wq);

    return static_cast<ssize_t>(len);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_close_write(ring_buffer* rb) {
    if (!rb) {
        return;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);
    rb->writer_closed = true;
    wake_reader_pollers_locked(rb);
    wake_writer_pollers_locked(rb);
    sync::spin_unlock_irqrestore(rb->lock, irq);

    sync::wake_all(rb->read_wq);
    sync::wake_all(rb->write_wq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_close_read(ring_buffer* rb) {
    if (!rb) {
        return;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);
    rb->reader_closed = true;
    wake_writer_pollers_locked(rb);
    wake_reader_pollers_locked(rb);
    sync::spin_unlock_irqrestore(rb->lock, irq);

    sync::wake_all(rb->write_wq);
    sync::wake_all(rb->read_wq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ring_buffer_take_marks(ring_buffer* rb, ring_buffer_mark_list& marks) {
    if (!rb) {
        return;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);
    while (ring_buffer_mark* mark = rb->marks.pop_front()) {
        marks.push_back(mark);
    }
    sync::spin_unlock_irqrestore(rb->lock, irq);
}

__PRIVILEGED_CODE uint32_t ring_buffer_poll_read(ring_buffer* rb, sync::poll_table* pt) {
    if (!rb) return 0;

    if (pt) {
        sync::poll_subscribe(*pt, rb->read_wq);
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);
    uint32_t mask = 0;
    if (readable_bytes(rb) > 0) {
        mask |= sync::POLL_IN;
    }
    if (is_shut(rb)) {
        mask |= sync::POLL_HUP;
    }
    sync::spin_unlock_irqrestore(rb->lock, irq);
    return mask;
}

__PRIVILEGED_CODE uint32_t ring_buffer_poll_write(ring_buffer* rb, sync::poll_table* pt) {
    if (!rb) return 0;

    if (pt) {
        sync::poll_subscribe(*pt, rb->write_wq);
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);
    uint32_t mask = 0;
    if (has_room(rb, 1, true)) {
        mask |= sync::POLL_OUT;
    }
    if (is_shut(rb)) {
        mask |= sync::POLL_ERR;
    }
    sync::spin_unlock_irqrestore(rb->lock, irq);
    return mask;
}
