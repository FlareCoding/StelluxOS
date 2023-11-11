#ifndef KMEMORY_H
#define KMEMORY_H
#include "kheap.h"

void memcpy(void* dest, const void* src, size_t size);

void memmove(void* dest, const void* src, size_t size);

int memcmp(void* dest, void* src, size_t size);

void memset(void* vaddr, uint8_t val, size_t size);

void zeromem(void* vaddr, size_t size);

void* allocPage();
void* zallocPage();

void* allocPages(size_t pages);
void* zallocPages(size_t pages);

void* kmalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t size);

// Placement new operator
void* operator new(size_t, void* ptr) noexcept;

// Placement delete operator (optional but recommended for symmetry)
void operator delete(void*, void*) noexcept;

// Global new ooperator
void* operator new(size_t size);

// Global delete operator
void operator delete(void* ptr) noexcept;

void operator delete(void* ptr, size_t) noexcept;

namespace kstl {
template <typename T>
class SharedPtr {
public:
    // Constructor
    explicit SharedPtr(T* ptr = nullptr) : m_ptr(ptr) {
        if (ptr) {
            m_refCount = new size_t(1);
        } else {
            m_refCount = nullptr;
        }
    }

    // Destructor
    ~SharedPtr() {
        release();
    }

    // Copy constructor
    SharedPtr(const SharedPtr& other) : m_ptr(other.m_ptr), m_refCount(other.m_refCount) {
        if (m_refCount) {
            ++(*m_refCount);
        }
    }

    // Copy assignment operator
    SharedPtr& operator=(const SharedPtr& other) {
        if (this != &other) {
            release();
            m_ptr = other.m_ptr;
            m_refCount = other.m_refCount;
            if (m_refCount) {
                ++(*m_refCount);
            }
        }
        return *this;
    }

    // Dereference operators
    T& operator*() const { return *m_ptr; }
    T* operator->() const { return m_ptr; }

    friend bool operator==(const SharedPtr<T>& lhs, const SharedPtr<T>& rhs) {
        return lhs.get() == rhs.get();
    }

    friend bool operator!=(const SharedPtr<T>& lhs, const SharedPtr<T>& rhs) {
        return !(lhs == rhs);
    }

    friend bool operator==(const SharedPtr<T>& lhs, const T* rhs) {
        return lhs.get() == rhs;
    }

    friend bool operator!=(const SharedPtr<T>& lhs, const T* rhs) {
        return !(lhs == rhs);
    }

    friend bool operator==(const T* lhs, const SharedPtr<T>& rhs) {
        return lhs == rhs.get();
    }

    friend bool operator!=(const T* lhs, const SharedPtr<T>& rhs) {
        return !(lhs == rhs);
    }

    // Utility functions
    size_t refCount() const {
        return m_refCount ? *m_refCount : 0;
    }

    T* get() const { return m_ptr; }

private:
    T* m_ptr;
    size_t* m_refCount;

    void release() {
        if (m_refCount && --(*m_refCount) == 0) {
            delete m_ptr;
            delete m_refCount;
        }
        m_ptr = nullptr;
        m_refCount = nullptr;
    }
};
} // namespace kstl

#endif
