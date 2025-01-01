#include <memory/memory.h>
#include <memory/allocators/heap_allocator.h>
#include <interrupts/irq.h>

EXTERN_C {
    int __cxa_atexit(void (*destructor) (void *), void *arg, void *dso_handle) {
        __unused destructor;
        __unused arg;
        __unused dso_handle;
        // We don't need to handle global object destruction right now
        return 0;
    }

    void *__dso_handle;
}

/**
 * @brief Sets the first `count` bytes of the memory area pointed to by `ptr` to the specified `value`.
 *
 * @param ptr Pointer to the memory area to be filled.
 * @param value The value to be set. Only the least significant byte is used.
 * @param count Number of bytes to be set to the value.
 * @return void* Pointer to the memory area `ptr`.
 */
void* memset(void* ptr, int value, size_t count) {
    unsigned char* byte_ptr = static_cast<unsigned char*>(ptr);
    unsigned char val = static_cast<unsigned char>(value);
    for (size_t i = 0; i < count; ++i) {
        byte_ptr[i] = val;
    }
    return ptr;
}

/**
 * @brief Copies `count` bytes from the memory area `src` to memory area `dest`.
 *
 * @param dest Pointer to the destination memory area where the content is to be copied.
 * @param src Pointer to the source memory area from which the content is to be copied.
 * @param count Number of bytes to copy.
 * @return void* Pointer to the destination memory area `dest`.
 */
void* memcpy(void* dest, const void* src, size_t count) {
    unsigned char* dest_ptr = static_cast<unsigned char*>(dest);
    const unsigned char* src_ptr = static_cast<const unsigned char*>(src);

    // If both pointers are aligned to word boundaries, copy in larger chunks for efficiency
    // This example assumes 32-bit architecture. Adjust as necessary for your architecture.
    #if defined(__i386__) || defined(__x86_64__) || defined(ARCH_X86_64)
    size_t word_size = sizeof(unsigned long);
    size_t i = 0;

    // Copy word-sized chunks
    for (; i + word_size <= count; i += word_size) {
        *reinterpret_cast<unsigned long*>(dest_ptr + i) = *reinterpret_cast<const unsigned long*>(src_ptr + i);
    }
    #endif

    // Copy any remaining bytes
    for (; i < count; ++i) {
        dest_ptr[i] = src_ptr[i];
    }

    return dest;
}

/**
 * @brief Compares the first `count` bytes of the memory areas `ptr1` and `ptr2`.
 *
 * @param ptr1 Pointer to the first memory area.
 * @param ptr2 Pointer to the second memory area.
 * @param count Number of bytes to compare.
 * @return int An integer less than, equal to, or greater than zero if the first `count` bytes of `ptr1` is found, respectively, to be less than, to match, or be greater than the first `count` bytes of `ptr2`.
 */
int memcmp(const void* ptr1, const void* ptr2, size_t count) {
    const unsigned char* byte_ptr1 = static_cast<const unsigned char*>(ptr1);
    const unsigned char* byte_ptr2 = static_cast<const unsigned char*>(ptr2);

    for (size_t i = 0; i < count; ++i) {
        if (byte_ptr1[i] != byte_ptr2[i]) {
            return byte_ptr1[i] - byte_ptr2[i];
        }
    }
    return 0;
}

void* operator new(size_t, void* ptr) noexcept {
    return ptr;
}

void* malloc(size_t size) {
    auto& heap = allocators::heap_allocator::get();
    void* ptr = heap.allocate(size);

#ifdef PROFILE_HEAP_CORRUPTION
    if (heap.detect_heap_corruption(true)) {
        panic("Kernel heap corrupted after malloc()");
    }
#endif

    return ptr;
}

void* zmalloc(size_t size) {
    void* ptr = malloc(size);

    if (!ptr) {
        return nullptr;
    }

    zeromem(ptr, size);
    return ptr;
}

void free(void* ptr) {
    if (!ptr) {
        return;
    }

    auto& heap = allocators::heap_allocator::get();
    heap.free(ptr);

#ifdef PROFILE_HEAP_CORRUPTION
    if (heap.detect_heap_corruption(true)) {
        panic("Kernel heap corrupted after free()");
    }
#endif
}

void* realloc(void* ptr, size_t size) {
    auto& heap = allocators::heap_allocator::get();
    void* res = heap.reallocate(ptr, size);

#ifdef PROFILE_HEAP_CORRUPTION
    if (heap.detect_heap_corruption(true)) {
        panic("Kernel heap corrupted after realloc()");
    }
#endif

    return res;
}

// Global new ooperator
void* operator new(size_t size) {
    return zmalloc(size);
}

// Global delete operator
void operator delete(void* ptr) noexcept {
    free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    free(ptr);
}

void* operator new[](size_t size) {
    return zmalloc(size);
}

void operator delete[](void* ptr) noexcept {
    free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    free(ptr);
}
