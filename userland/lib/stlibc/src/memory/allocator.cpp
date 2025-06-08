#include <stlibc/memory/allocator.hpp>
#include <stlibc/memory/mman.h>
#include <stlibc/system/syscall.h>

namespace stlibc {
namespace memory {

void* heap_allocator::allocate(size_t size) {
    if (size == 0) {
        return nullptr;
    }

    // Round up to minimum allocation size
    if (size < MIN_ALLOC_SIZE) {
        size = MIN_ALLOC_SIZE;
    }

    // Calculate number of pages needed
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Allocate memory with read/write permissions
    void* ptr = allocate_pages(pages);
    if (!ptr) {
        return nullptr;
    }

    return ptr;
}

void heap_allocator::deallocate(void* ptr, size_t size) {
    if (!ptr) {
        return;
    }

    // Calculate number of pages to free
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    deallocate_pages(ptr, pages);
}

void* heap_allocator::reallocate(void* ptr, size_t old_size, size_t new_size) {
    if (!ptr) {
        return allocate(new_size);
    }

    if (new_size == 0) {
        deallocate(ptr, old_size);
        return nullptr;
    }

    // Calculate old and new page counts
    size_t old_pages = (old_size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t new_pages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;

    // If same number of pages, just return the same pointer
    if (old_pages == new_pages) {
        return ptr;
    }

    // Allocate new memory
    void* new_ptr = allocate_pages(new_pages);
    if (!new_ptr) {
        return nullptr;
    }

    // Copy old data
    size_t copy_size = old_size < new_size ? old_size : new_size;
    for (size_t i = 0; i < copy_size; i++) {
        ((char*)new_ptr)[i] = ((char*)ptr)[i];
    }

    // Free old memory
    deallocate_pages(ptr, old_pages);

    return new_ptr;
}

void* heap_allocator::allocate_pages(size_t pages) {
    return mmap(nullptr, pages * PAGE_SIZE, PROT_READ | PROT_WRITE, 0);
}

void heap_allocator::deallocate_pages(void* ptr, size_t pages) {
    munmap(ptr, pages * PAGE_SIZE);
}

} // namespace memory
} // namespace stlibc 