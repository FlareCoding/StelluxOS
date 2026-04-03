#ifndef STELLUX_RC_STRONG_REF_H
#define STELLUX_RC_STRONG_REF_H

#include "common/types.h"
#include "mm/heap.h"

namespace rc {

struct adopt_ref_t { explicit constexpr adopt_ref_t() = default; };
inline constexpr adopt_ref_t ADOPT_REF{};

/**
 * RAII owning smart pointer for ref_counted objects.
 *
 * Copy increments the refcount. Move steals without touching the count.
 * When the last strong_ref is destroyed, T::ref_destroy(T*) is called,
 * giving the type full control over inline vs deferred destruction.
 */
template<typename T>
class strong_ref {
public:
    constexpr strong_ref() noexcept : m_ptr(nullptr) {}

    strong_ref(const strong_ref& other) noexcept : m_ptr(other.m_ptr) {
        if (m_ptr) {
            m_ptr->add_ref();
        }
    }

    strong_ref(strong_ref&& other) noexcept : m_ptr(other.m_ptr) {
        other.m_ptr = nullptr;
    }

    template<typename U>
    strong_ref(const strong_ref<U>& other) noexcept
        : m_ptr(static_cast<T*>(other.ptr())) {
        if (m_ptr) {
            m_ptr->add_ref();
        }
    }

    template<typename U>
    strong_ref(strong_ref<U>&& other) noexcept
        : m_ptr(static_cast<T*>(other.m_ptr)) {
        other.m_ptr = nullptr;
    }

    strong_ref& operator=(const strong_ref& other) noexcept {
        if (this == &other) {
            return *this;
        }
        T* new_ptr = other.m_ptr;
        if (new_ptr) {
            new_ptr->add_ref();
        }
        reset();
        m_ptr = new_ptr;
        return *this;
    }

    strong_ref& operator=(strong_ref&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        m_ptr = other.m_ptr;
        other.m_ptr = nullptr;
        return *this;
    }

    ~strong_ref() noexcept {
        reset();
    }

    /**
     * Wrap a raw pointer whose refcount is already 1 (from allocation).
     * Does NOT call add_ref.
     */
    [[nodiscard]] static strong_ref adopt(T* raw) noexcept {
        return strong_ref(raw, ADOPT_REF);
    }

    /**
     * Try to acquire a reference from a raw pointer that may be dying.
     * Uses try_add_ref (CAS loop); returns null ref if refcount is 0.
     */
    [[nodiscard]] static strong_ref try_from_raw(T* raw) noexcept {
        if (!raw) {
            return strong_ref();
        }
        if (!raw->try_add_ref()) {
            return strong_ref();
        }
        return strong_ref(raw, ADOPT_REF);
    }

    [[nodiscard]] T* ptr() const noexcept { return m_ptr; }
    [[nodiscard]] T* operator->() const noexcept { return m_ptr; }
    [[nodiscard]] T& operator*() const noexcept { return *m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

    void reset() noexcept {
        T* dying = m_ptr;
        m_ptr = nullptr;
        if (dying && dying->release()) {
            T::ref_destroy(dying);
        }
    }

    void swap(strong_ref& other) noexcept {
        T* tmp = m_ptr;
        m_ptr = other.m_ptr;
        other.m_ptr = tmp;
    }

private:
    template<typename U>
    friend class strong_ref;
    explicit constexpr strong_ref(T* raw, adopt_ref_t) noexcept : m_ptr(raw) {}

    T* m_ptr;
};

template<typename T>
inline bool operator==(const strong_ref<T>& a, const strong_ref<T>& b) noexcept {
    return a.ptr() == b.ptr();
}

template<typename T>
inline bool operator!=(const strong_ref<T>& a, const strong_ref<T>& b) noexcept {
    return a.ptr() != b.ptr();
}

/**
 * Allocate from the privileged heap and construct a ref_counted object.
 * Returns null strong_ref on allocation failure.
 * @note Privilege: **required**
 */
template<typename T, typename... args_t>
[[nodiscard]] __PRIVILEGED_CODE
strong_ref<T> make_kref(args_t&&... args) noexcept {
    void* mem = heap::kzalloc(sizeof(T));
    if (!mem) {
        return strong_ref<T>();
    }
    T* obj = new (mem) T(static_cast<args_t&&>(args)...);
    return strong_ref<T>::adopt(obj);
}

/**
 * Allocate from the unprivileged heap and construct a ref_counted object.
 * Auto-elevates if called from unprivileged context.
 * Returns null strong_ref on allocation failure.
 */
template<typename T, typename... args_t>
[[nodiscard]]
strong_ref<T> make_uref(args_t&&... args) noexcept {
    void* mem = heap::uzalloc(sizeof(T));
    if (!mem) {
        return strong_ref<T>();
    }
    T* obj = new (mem) T(static_cast<args_t&&>(args)...);
    return strong_ref<T>::adopt(obj);
}

} // namespace rc

#endif // STELLUX_RC_STRONG_REF_H
