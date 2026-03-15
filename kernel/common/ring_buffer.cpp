#include "common/ring_buffer.h"
#include "mm/heap.h"
#include "common/string.h"
#include "sched/sched.h"

static inline size_t readable_bytes(const ring_buffer* rb) {
    return (rb->head - rb->tail) & (rb->capacity - 1);
}

static inline size_t writable_bytes(const ring_buffer* rb) {
    return rb->capacity - 1 - readable_bytes(rb);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ring_buffer* ring_buffer_create(size_t capacity) {
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
__PRIVILEGED_CODE ssize_t ring_buffer_read(ring_buffer* rb, uint8_t* buf, size_t len, bool nonblock) {
    if (!rb || !buf || len == 0) {
        return RB_ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);

    if (readable_bytes(rb) == 0 && !rb->writer_closed) {
        if (nonblock) {
            sync::spin_unlock_irqrestore(rb->lock, irq);
            return RB_ERR_AGAIN;
        }
        while (readable_bytes(rb) == 0 && !rb->writer_closed && !sched::is_kill_pending()) {
            irq = sync::wait(rb->read_wq, rb->lock, irq);
        }
    }

    size_t avail = readable_bytes(rb);
    if (avail == 0) {
        sync::spin_unlock_irqrestore(rb->lock, irq);
        return 0; // EOF
    }

    size_t to_read = avail < len ? avail : len;
    size_t tail_idx = rb->tail & (rb->capacity - 1);
    size_t first = rb->capacity - tail_idx;
    if (first > to_read) {
        first = to_read;
    }

    string::memcpy(buf, rb->data + tail_idx, first);
    if (first < to_read) {
        string::memcpy(buf + first, rb->data, to_read - first);
    }

    rb->tail += to_read;

    sync::spin_unlock_irqrestore(rb->lock, irq);
    sync::wake_one(rb->write_wq);

    return static_cast<ssize_t>(to_read);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t ring_buffer_write(ring_buffer* rb, const uint8_t* buf, size_t len, bool nonblock) {
    if (!rb || !buf || len == 0) {
        return RB_ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(rb->lock);

    if (writable_bytes(rb) == 0 && !rb->reader_closed) {
        if (nonblock) {
            sync::spin_unlock_irqrestore(rb->lock, irq);
            return RB_ERR_AGAIN;
        }
        while (writable_bytes(rb) == 0 && !rb->reader_closed && !sched::is_kill_pending()) {
            irq = sync::wait(rb->write_wq, rb->lock, irq);
        }
    }

    if (rb->reader_closed || sched::is_kill_pending()) {
        sync::spin_unlock_irqrestore(rb->lock, irq);
        return RB_ERR_PIPE;
    }

    size_t space = writable_bytes(rb);
    size_t to_write = space < len ? space : len;
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

    sync::spin_unlock_irqrestore(rb->lock, irq);
    sync::wake_one(rb->read_wq);

    return static_cast<ssize_t>(to_write);
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
    sync::spin_unlock_irqrestore(rb->lock, irq);

    sync::wake_all(rb->read_wq);
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
    sync::spin_unlock_irqrestore(rb->lock, irq);

    sync::wake_all(rb->write_wq);
}
