#ifndef STELLUX_RC_REF_COUNTED_H
#define STELLUX_RC_REF_COUNTED_H

#include "common/types.h"
#include "common/logging.h"
#include "sync/atomic.h"

namespace rc {

constexpr uint32_t REFCOUNT_SATURATED = 0xC0000000u;
constexpr uint32_t REFCOUNT_POISON    = 0xDEADBEEFu;

/**
 * Intrusive CRTP base class for reference-counted kernel objects.
 *
 * Derived types inherit from ref_counted<Derived> and get an embedded
 * atomic refcount initialized to 1. The refcount is managed by
 * strong_ref<T> -- callers should rarely touch add_ref/release directly.
 *
 * Contract: every type T used with strong_ref<T> must also provide
 *   static void T::ref_destroy(T* self);
 * which is called when the last strong_ref is released.
 */
template<typename T>
class ref_counted {
public:
    ref_counted(const ref_counted&) = delete;
    ref_counted& operator=(const ref_counted&) = delete;
    ref_counted(ref_counted&&) = delete;
    ref_counted& operator=(ref_counted&&) = delete;

    /**
     * Increment the reference count.
     * Caller must already hold a valid reference.
     * Saturates at REFCOUNT_SATURATED instead of wrapping.
     */
    void add_ref() const {
        uint32_t cur = m_refcount.load_relaxed();

        if (cur == REFCOUNT_POISON) {
            log::fatal("rc: add_ref on poisoned object %p", this);
        }

        if (cur == 0) {
            log::fatal("rc: add_ref on zero-ref object %p", this);
        }

        if (cur >= REFCOUNT_SATURATED) {
            log::warn("rc: refcount saturated on %p", this);
            return;
        }

        m_refcount.fetch_add_relaxed(1);
    }

    /**
     * Attempt to increment the refcount from a raw pointer.
     * Fails atomically if the count is already 0 (object dying).
     * @return true if a reference was acquired, false if the object is dead.
     */
    [[nodiscard]] bool try_add_ref() const {
        uint32_t expected = m_refcount.load_relaxed();
        for (;;) {
            if (expected == 0) {
                return false;
            }

            if (expected == REFCOUNT_POISON) {
                return false;
            }

            if (expected >= REFCOUNT_SATURATED) {
                return true;
            }

            if (m_refcount.cmpxchg_weak_acquire(expected, expected + 1)) {
                return true;
            }
        }
    }

    /**
     * Decrement the reference count.
     * @return true if this was the last reference (caller must destroy).
     *
     * Uses RELEASE ordering on the decrement. When the last ref drops,
     * an ACQUIRE fence ensures all prior accesses are visible before
     * destruction.
     */
    [[nodiscard]] bool release() const {
        uint32_t old = m_refcount.fetch_sub_release(1);
        if (old == REFCOUNT_POISON) {
            log::fatal("rc: release on poisoned object %p", this);
        }

        if (old == 0) {
            log::fatal("rc: release underflow on %p", this);
        }

        if (old >= REFCOUNT_SATURATED) {
            m_refcount.store_relaxed(REFCOUNT_SATURATED);
            return false;
        }

        if (old == 1) {
            sync::atomic_fence_acquire();
            m_refcount.store_relaxed(REFCOUNT_POISON);
            return true;
        }

        return false;
    }

    /**
     * Read the current refcount (relaxed load, for debugging only).
     */
    [[nodiscard]] uint32_t ref_count() const {
        return m_refcount.load_relaxed();
    }

protected:
    ref_counted() : m_refcount{1} {}
    ~ref_counted() = default;

private:
    mutable sync::atomic<uint32_t> m_refcount;
};

} // namespace rc

#endif // STELLUX_RC_REF_COUNTED_H
