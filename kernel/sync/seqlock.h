#ifndef STELLUX_SYNC_SEQLOCK_H
#define STELLUX_SYNC_SEQLOCK_H

#include "common/types.h"
#include "hw/cpu.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"

namespace sync {

/**
 * A small plain value read on every fast path and rarely changed. Readers take
 * a lock-free snapshot and retry only if a write overlapped, so lowered code can
 * read it. Writers serialize with interrupts off, keeping every retry short.
 */
template<typename T>
class seqlocked {
    static_assert(__is_trivially_copyable(T), "seqlocked<T> copies T as plain memory");

public:
    constexpr seqlocked() : m_sequence(0), m_writer_lock(SPINLOCK_INIT), m_value() {}
    constexpr explicit seqlocked(const T& initial)
        : m_sequence(0), m_writer_lock(SPINLOCK_INIT), m_value(initial) {}

    seqlocked(const seqlocked&) = delete;
    seqlocked& operator=(const seqlocked&) = delete;

    [[nodiscard]] T read() const {
        while (true) {
            uint32_t start = m_sequence.load_acquire();
            if (start & 1) {
                cpu::relax();
                continue;
            }

            T copy = m_value;

            atomic_fence_acquire();
            if (m_sequence.load_relaxed() == start) {
                return copy;
            }
        }
    }

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void write(const T& value) {
        irq_lock_guard guard(m_writer_lock);

        m_sequence.store_relaxed(m_sequence.load_relaxed() + 1);
        atomic_fence_release();

        m_value = value;

        m_sequence.store_release(m_sequence.load_relaxed() + 1);
    }

private:
    atomic<uint32_t> m_sequence; // Odd while a write is in progress
    spinlock         m_writer_lock;
    T                m_value;
};

} // namespace sync

#endif // STELLUX_SYNC_SEQLOCK_H
