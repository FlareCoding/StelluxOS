#ifndef MEMORY_H
#define MEMORY_H
#include <types.h>
#include <type_traits>

/**
 * @brief Sets the first `count` bytes of the memory area pointed to by `ptr` to the specified `value`.
 *
 * @param ptr Pointer to the memory area to be filled.
 * @param value The value to be set. The value is passed as an `int`, but the function fills the memory using the unsigned char conversion of this value.
 * @param count Number of bytes to be set to the value.
 * @return void* Pointer to the memory area `ptr`.
 */
void* memset(void* ptr, int value, size_t count);

/**
 * @brief Copies `count` bytes from the memory area `src` to memory area `dest`.
 *
 * @param dest Pointer to the destination memory area where the content is to be copied.
 * @param src Pointer to the source memory area from which the content is to be copied.
 * @param count Number of bytes to copy.
 * @return void* Pointer to the destination memory area `dest`.
 */
void* memcpy(void* dest, const void* src, size_t count);

/**
 * @brief Compares the first `count` bytes of the memory areas `ptr1` and `ptr2`.
 *
 * @param ptr1 Pointer to the first memory area.
 * @param ptr2 Pointer to the second memory area.
 * @param count Number of bytes to compare.
 * @return int An integer less than, equal to, or greater than zero if the first `count` bytes of `ptr1` is found, respectively, to be less than, to match, or be greater than the first `count` bytes of `ptr2`.
 */
int memcmp(const void* ptr1, const void* ptr2, size_t count);

// Placement new operator
void* operator new(size_t, void* ptr) noexcept;

#define zeromem(vaddr, size) memset(vaddr, 0, size)

#define GENERATE_STATIC_SINGLETON(type) \
    __PRIVILEGED_DATA alignas(type) static uint8_t buffer[sizeof(type)]; \
    __PRIVILEGED_DATA static type* instance = nullptr; \
    \
    if (!instance) { \
        instance = new (buffer) type(); \
    } \
    return *instance;

void* malloc(size_t size);
void* zmalloc(size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t size);

// Global new ooperator
void* operator new(size_t size);

// Global delete operator
void operator delete(void* ptr) noexcept;
void operator delete(void* ptr, size_t) noexcept;

void* operator new[](size_t size);
void operator delete[](void* ptr) noexcept;
void operator delete[](void* ptr, size_t) noexcept;

namespace kstl {
template <typename T>
class shared_ptr {
// Make every instantiation of shared_ptr a friend
template <typename U> friend class shared_ptr;

public:
    // Default constructor
    explicit shared_ptr(T* ptr = nullptr) 
        : m_ptr(ptr), m_ref_count(ptr ? new size_t(1) : nullptr) {}

    // Destructor
    ~shared_ptr() noexcept {
        release_resources();
    }

    // Copy constructor
    shared_ptr(const shared_ptr& other) 
        : m_ptr(other.m_ptr), m_ref_count(other.m_ref_count) {
        if (m_ref_count) {
            ++(*m_ref_count);
        }
    }

    // Upcast support from <Derived> to <Base>
    template <typename U>
    shared_ptr(const shared_ptr<U>& other,
               typename std::enable_if<std::is_convertible<U*, T*>::value>::type* = 0)
        : m_ptr(other.m_ptr),
          m_ref_count(other.m_ref_count)
    {
        if (m_ref_count) {
            ++(*m_ref_count);
        }
    }

    // Copy assignment operator
    shared_ptr& operator=(const shared_ptr& other) {
        if (this != &other) {
            release_resources();
            m_ptr = other.m_ptr;
            m_ref_count = other.m_ref_count;
            if (m_ref_count) {
                ++(*m_ref_count);
            }
        }
        return *this;
    }

    // Move constructor
    shared_ptr(shared_ptr&& other) noexcept 
        : m_ptr(other.m_ptr), m_ref_count(other.m_ref_count) {
        other.m_ptr = nullptr;
        other.m_ref_count = nullptr;
    }

    template <typename U>
    shared_ptr(const shared_ptr<U>& other, T* casted_ptr)
        : m_ptr(casted_ptr),
        m_ref_count(other.m_ref_count) {
        if (m_ref_count) {
            ++(*m_ref_count);
        }
    }

    // Move assignment operator
    shared_ptr& operator=(shared_ptr&& other) noexcept {
        if (this != &other) {
            release_resources();
            m_ptr = other.m_ptr;
            m_ref_count = other.m_ref_count;
            other.m_ptr = nullptr;
            other.m_ref_count = nullptr;
        }
        return *this;
    }

    // Dereference operators
    T& operator*() const { 
        return *m_ptr; 
    }

    T* operator->() const { 
        return m_ptr; 
    }

    // Comparison operators
    friend bool operator==(const shared_ptr<T>& lhs, const shared_ptr<T>& rhs) {
        return lhs.get() == rhs.get();
    }

    friend bool operator!=(const shared_ptr<T>& lhs, const shared_ptr<T>& rhs) {
        return !(lhs == rhs);
    }

    friend bool operator==(const shared_ptr<T>& lhs, const T* rhs) {
        return lhs.get() == rhs;
    }

    friend bool operator!=(const shared_ptr<T>& lhs, const T* rhs) {
        return !(lhs == rhs);
    }

    friend bool operator==(const T* lhs, const shared_ptr<T>& rhs) {
        return lhs == rhs.get();
    }

    friend bool operator!=(const T* lhs, const shared_ptr<T>& rhs) {
        return !(lhs == rhs);
    }

    explicit operator bool() const noexcept {
        return m_ptr != nullptr;
    }

    // Utility functions
    size_t ref_count() const {
        return m_ref_count ? *m_ref_count : 0;
    }

    T* get() const { 
        return m_ptr; 
    }

private:
    T* m_ptr;
    size_t* m_ref_count;

    void release_resources() noexcept {
        if (m_ref_count && --(*m_ref_count) == 0) {
            delete m_ptr;
            delete m_ref_count;
        }
        m_ptr = nullptr;
        m_ref_count = nullptr;
    }
};

template <typename T, typename... Args>
shared_ptr<T> make_shared(Args&&... args) {
    // Construct the T object on the heap.
    T* ptr = new T(static_cast<Args&&>(args)...);

    // Return a shared_ptr that takes ownership of this new object.
    return shared_ptr<T>(ptr);
}

// Static cast for shared_ptr
template <class T, class U>
kstl::shared_ptr<T> static_pointer_cast(const kstl::shared_ptr<U>& r) noexcept {
    return kstl::shared_ptr<T>(r, static_cast<T*>(r.get())); // Share ownership but cast the pointer
}

// Dynamic cast for shared_ptr
template <class T, class U>
kstl::shared_ptr<T> dynamic_pointer_cast(const kstl::shared_ptr<U>& r) noexcept {
    if (T* p = dynamic_cast<T*>(r.get())) {
        return kstl::shared_ptr<T>(r, p); // Share ownership only if the cast succeeds
    }
    return kstl::shared_ptr<T>(); // Return an empty shared_ptr if cast fails
}

// Reinterpret cast for shared_ptr
template <class T, class U>
kstl::shared_ptr<T> reinterpret_pointer_cast(const kstl::shared_ptr<U>& r) noexcept {
    return kstl::shared_ptr<T>(r, reinterpret_cast<T*>(r.get())); // Share ownership but reinterpret the pointer
}
} // namespace kstl

#endif // MEMORY_H
