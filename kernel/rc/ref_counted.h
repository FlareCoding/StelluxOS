#ifndef STELLUX_RC_REF_COUNTED_H
#define STELLUX_RC_REF_COUNTED_H

#include "common/types.h"
#include "common/logging.h"

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
        uint32_t cur = __atomic_load_n(&m_refcount, __ATOMIC_RELAXED);

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

        __atomic_fetch_add(&m_refcount, 1, __ATOMIC_RELAXED);
    }

    /**
     * Attempt to increment the refcount from a raw pointer.
     * Fails atomically if the count is already 0 (object dying).
     * @return true if a reference was acquired, false if the object is dead.
     */
    [[nodiscard]] bool try_add_ref() const {
        uint32_t expected = __atomic_load_n(&m_refcount, __ATOMIC_RELAXED);
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

            if (__atomic_compare_exchange_n(&m_refcount, &expected, expected + 1,
                                            true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
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
        uint32_t old = __atomic_fetch_sub(&m_refcount, 1, __ATOMIC_RELEASE);

        if (old == REFCOUNT_POISON) {
            log::fatal("rc: release on poisoned object %p", this);
        }
        if (old == 0) {
            log::fatal("rc: release underflow on %p", this);
        }
        if (old >= REFCOUNT_SATURATED) {
            __atomic_store_n(&m_refcount, REFCOUNT_SATURATED, __ATOMIC_RELAXED);
            return false;
        }
        if (old == 1) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            __atomic_store_n(&m_refcount, REFCOUNT_POISON, __ATOMIC_RELAXED);
            return true;
        }

        return false;
    }

    /**
     * Read the current refcount (relaxed load, for debugging only).
     */
    [[nodiscard]] uint32_t ref_count() const {
        return __atomic_load_n(&m_refcount, __ATOMIC_RELAXED);
    }

protected:
    ref_counted() : m_refcount(1) {}
    ~ref_counted() = default;

private:
    mutable uint32_t m_refcount;
};

} // namespace rc

#endif // STELLUX_RC_REF_COUNTED_H
