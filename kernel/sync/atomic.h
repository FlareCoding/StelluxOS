#ifndef STELLUX_SYNC_ATOMIC_H
#define STELLUX_SYNC_ATOMIC_H

namespace sync {

template<typename T>
inline constexpr bool is_lock_free_atomic_value =
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8) &&
    (__is_integral(T) || __is_enum(T) || __is_pointer(T)) &&
    __atomic_always_lock_free(sizeof(T), nullptr);

template<typename T>
inline constexpr bool is_atomic_arith = __is_integral(T) && !__is_same(T, bool);

namespace atomic_detail {

template<typename T, int order>
[[nodiscard]] inline T load(const T* p) {
    return __atomic_load_n(p, order);
}

template<typename T, int order>
inline void store(T* p, T v) {
    __atomic_store_n(p, v, order);
}

template<typename T, int order>
[[nodiscard]] inline T exchange(T* p, T v) {
    return __atomic_exchange_n(p, v, order);
}

template<typename T, int order>
inline T fetch_add(T* p, T v) {
    return __atomic_fetch_add(p, v, order);
}

template<typename T, int order>
inline T fetch_sub(T* p, T v) {
    return __atomic_fetch_sub(p, v, order);
}

template<typename T, int order>
inline T fetch_and(T* p, T v) {
    return __atomic_fetch_and(p, v, order);
}

template<typename T, int order>
inline T fetch_or(T* p, T v) {
    return __atomic_fetch_or(p, v, order);
}

template<typename T, int order>
inline T fetch_xor(T* p, T v) {
    return __atomic_fetch_xor(p, v, order);
}

template<typename T, int success_order, bool weak>
[[nodiscard]] inline bool cmpxchg(T* p, T& expected, T desired) {
    return __atomic_compare_exchange_n(
        p, &expected, desired, weak, success_order, __ATOMIC_RELAXED);
}

/**
 * @brief Shared atomic operations. self_t provides addr().
 */
template<typename T, typename self_t>
class atomic_ops {
    T* ptr() const {
        return static_cast<const self_t*>(this)->addr();
    }

public:
    [[nodiscard]] T load_relaxed() const {
        return load<T, __ATOMIC_RELAXED>(ptr());
    }

    [[nodiscard]] T load_acquire() const {
        return load<T, __ATOMIC_ACQUIRE>(ptr());
    }

    void store_relaxed(T v) const {
        store<T, __ATOMIC_RELAXED>(ptr(), v);
    }

    void store_release(T v) const {
        store<T, __ATOMIC_RELEASE>(ptr(), v);
    }

    [[nodiscard]] T exchange_relaxed(T v) const {
        return exchange<T, __ATOMIC_RELAXED>(ptr(), v);
    }

    [[nodiscard]] T exchange_acquire(T v) const {
        return exchange<T, __ATOMIC_ACQUIRE>(ptr(), v);
    }

    [[nodiscard]] T exchange_release(T v) const {
        return exchange<T, __ATOMIC_RELEASE>(ptr(), v);
    }

    [[nodiscard]] T exchange_acq_rel(T v) const {
        return exchange<T, __ATOMIC_ACQ_REL>(ptr(), v);
    }

    T fetch_add_relaxed(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_add<T, __ATOMIC_RELAXED>(ptr(), v);
    }

    T fetch_add_acquire(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_add<T, __ATOMIC_ACQUIRE>(ptr(), v);
    }

    T fetch_add_release(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_add<T, __ATOMIC_RELEASE>(ptr(), v);
    }

    T fetch_add_acq_rel(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_add<T, __ATOMIC_ACQ_REL>(ptr(), v);
    }

    T fetch_sub_relaxed(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_sub<T, __ATOMIC_RELAXED>(ptr(), v);
    }

    T fetch_sub_acquire(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_sub<T, __ATOMIC_ACQUIRE>(ptr(), v);
    }

    T fetch_sub_release(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_sub<T, __ATOMIC_RELEASE>(ptr(), v);
    }

    T fetch_sub_acq_rel(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_sub<T, __ATOMIC_ACQ_REL>(ptr(), v);
    }

    T fetch_and_relaxed(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_and<T, __ATOMIC_RELAXED>(ptr(), v);
    }

    T fetch_and_acquire(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_and<T, __ATOMIC_ACQUIRE>(ptr(), v);
    }

    T fetch_and_release(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_and<T, __ATOMIC_RELEASE>(ptr(), v);
    }

    T fetch_and_acq_rel(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_and<T, __ATOMIC_ACQ_REL>(ptr(), v);
    }

    T fetch_or_relaxed(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_or<T, __ATOMIC_RELAXED>(ptr(), v);
    }

    T fetch_or_acquire(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_or<T, __ATOMIC_ACQUIRE>(ptr(), v);
    }

    T fetch_or_release(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_or<T, __ATOMIC_RELEASE>(ptr(), v);
    }

    T fetch_or_acq_rel(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_or<T, __ATOMIC_ACQ_REL>(ptr(), v);
    }

    T fetch_xor_relaxed(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_xor<T, __ATOMIC_RELAXED>(ptr(), v);
    }

    T fetch_xor_acquire(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_xor<T, __ATOMIC_ACQUIRE>(ptr(), v);
    }

    T fetch_xor_release(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_xor<T, __ATOMIC_RELEASE>(ptr(), v);
    }

    T fetch_xor_acq_rel(T v) const
        requires (is_atomic_arith<T>) {
        return fetch_xor<T, __ATOMIC_ACQ_REL>(ptr(), v);
    }

    [[nodiscard]] bool cmpxchg_weak_relaxed(T& expected, T desired) const {
        return cmpxchg<T, __ATOMIC_RELAXED, true>(ptr(), expected, desired);
    }

    [[nodiscard]] bool cmpxchg_weak_acquire(T& expected, T desired) const {
        return cmpxchg<T, __ATOMIC_ACQUIRE, true>(ptr(), expected, desired);
    }

    [[nodiscard]] bool cmpxchg_weak_release(T& expected, T desired) const {
        return cmpxchg<T, __ATOMIC_RELEASE, true>(ptr(), expected, desired);
    }

    [[nodiscard]] bool cmpxchg_weak_acq_rel(T& expected, T desired) const {
        return cmpxchg<T, __ATOMIC_ACQ_REL, true>(ptr(), expected, desired);
    }

    [[nodiscard]] bool cmpxchg_strong_relaxed(T& expected, T desired) const {
        return cmpxchg<T, __ATOMIC_RELAXED, false>(ptr(), expected, desired);
    }

    [[nodiscard]] bool cmpxchg_strong_acquire(T& expected, T desired) const {
        return cmpxchg<T, __ATOMIC_ACQUIRE, false>(ptr(), expected, desired);
    }

    [[nodiscard]] bool cmpxchg_strong_release(T& expected, T desired) const {
        return cmpxchg<T, __ATOMIC_RELEASE, false>(ptr(), expected, desired);
    }

    [[nodiscard]] bool cmpxchg_strong_acq_rel(T& expected, T desired) const {
        return cmpxchg<T, __ATOMIC_ACQ_REL, false>(ptr(), expected, desired);
    }
};

} // namespace atomic_detail

/**
 * @brief Owned lock free atomic cell.
 *
 * Order is part of the method name. CAS failure is always relaxed.
 * The default constructor zero initializes the value, so a global
 * atomic still lands in BSS. fetch_* returns the previous value.
 * T is an integer, enum, or pointer of 1, 2, 4, or 8 bytes and must
 * be lock free (no libatomic).
 */
template<typename T>
class atomic : public atomic_detail::atomic_ops<T, atomic<T>> {
    static_assert(is_lock_free_atomic_value<T>,
                  "atomic<T> needs a lock free 1, 2, 4, or 8 byte integer, enum, or pointer");

public:
    atomic() = default;
    explicit constexpr atomic(T value) : m_value(value) {}

    atomic(const atomic&) = delete;
    atomic& operator=(const atomic&) = delete;
    atomic(atomic&&) = delete;
    atomic& operator=(atomic&&) = delete;

private:
    friend class atomic_detail::atomic_ops<T, atomic<T>>;

    T* addr() const {
        static_assert(sizeof(atomic) == sizeof(T));
        static_assert(alignof(atomic) == alignof(T));

        return &m_value;
    }

    mutable T m_value{};
};

/**
 * @brief Borrowed atomic view of existing storage.
 *
 * Use for storage that must stay a plain scalar, such as futex words
 * in user memory or fields of structs that must remain copyable.
 * The referenced object must outlive the view and be naturally aligned.
 * Copying the view is fine. Assigning a view is deleted so a store is
 * never written as operator=.
 */
template<typename T>
class atomic_ref : public atomic_detail::atomic_ops<T, atomic_ref<T>> {
    static_assert(is_lock_free_atomic_value<T>,
                  "atomic_ref<T> needs a lock free 1, 2, 4, or 8 byte integer, enum, or pointer");

public:
    explicit atomic_ref(T& obj) : m_ptr(&obj) {}

    atomic_ref(const atomic_ref&) = default;
    atomic_ref& operator=(const atomic_ref&) = delete;

private:
    friend class atomic_detail::atomic_ops<T, atomic_ref<T>>;

    T* addr() const {
        return m_ptr;
    }

    T* m_ptr;
};

/**
 * @brief Acquire fence pairing with a prior release RMW.
 *
 * Call this when a refcount drop observes the last reference, before
 * destroying the object.
 */
inline void atomic_fence_acquire() {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

/**
 * @brief Release fence pairing with a later acquire load or RMW.
 */
inline void atomic_fence_release() {
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

} // namespace sync

#endif // STELLUX_SYNC_ATOMIC_H
