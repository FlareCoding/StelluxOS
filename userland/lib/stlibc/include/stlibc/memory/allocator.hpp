#ifndef STELLUX_ALLOCATOR_HPP
#define STELLUX_ALLOCATOR_HPP

#include <stlibc/stddef.h>

namespace stlibc {
namespace memory {

class heap_allocator {
public:
    static void* allocate(size_t size);
    static void deallocate(void* ptr, size_t size);
    static void* reallocate(void* ptr, size_t old_size, size_t new_size);

private:
    static constexpr size_t PAGE_SIZE = 4096;
    static constexpr size_t MIN_ALLOC_SIZE = 16;
    static constexpr size_t MAX_ALLOC_SIZE = 1024 * 1024; // 1MB

    // Internal memory management functions
    static void* allocate_pages(size_t pages);
    static void deallocate_pages(void* ptr, size_t pages);
};

} // namespace memory
} // namespace stlibc

#endif // STELLUX_ALLOCATOR_HPP 