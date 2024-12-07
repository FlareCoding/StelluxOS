#include <memory/allocators/page_bootstrap_allocator.h>
#include <memory/paging.h>
#include <memory/memory.h>

namespace allocators {
page_bootstrap_allocator& page_bootstrap_allocator::get() {
    GENERATE_STATIC_SINGLETON(page_bootstrap_allocator);
}

__PRIVILEGED_CODE void page_bootstrap_allocator::init(uintptr_t base, size_t size) {
    m_base_address = base;
    m_free_pointer = m_base_address;
    m_end_address = m_base_address + size;

    zeromem((void*)base, size);
}

__PRIVILEGED_CODE void page_bootstrap_allocator::lock_page(void* addr) {
    // No-op: Bootstrap allocator does not track individual page usage
    __unused addr;
}

__PRIVILEGED_CODE void page_bootstrap_allocator::lock_pages(void* addr, size_t count) {
    // No-op: Bootstrap allocator does not track individual page usage
    __unused addr;
    __unused count;
}

__PRIVILEGED_CODE void page_bootstrap_allocator::free_page(void* addr) {
    // No-op: Bootstrap allocator does not support freeing pages
    __unused addr;
}

__PRIVILEGED_CODE void page_bootstrap_allocator::free_pages(void* addr, size_t count) {
    // No-op: Bootstrap allocator does not support freeing pages
    __unused addr;
    __unused count;
}

__PRIVILEGED_CODE void* page_bootstrap_allocator::alloc_page() {
    if (m_free_pointer + PAGE_SIZE > m_end_address) {
        return nullptr; // Out of memory
    }
    void* allocated_page = reinterpret_cast<void*>(m_free_pointer);
    m_free_pointer += PAGE_SIZE;
    return allocated_page;
}

__PRIVILEGED_CODE void* page_bootstrap_allocator::alloc_pages(size_t count) {
    uintptr_t required_size = count * PAGE_SIZE;
    if (m_free_pointer + required_size > m_end_address) {
        return nullptr; // Out of memory
    }
    void* allocated_pages = reinterpret_cast<void*>(m_free_pointer);
    m_free_pointer += required_size;
    return allocated_pages;
}

__PRIVILEGED_CODE void* page_bootstrap_allocator::alloc_pages_aligned(size_t count, uint64_t alignment) {
    if (alignment < PAGE_SIZE) {
        alignment = PAGE_SIZE; // Alignment must be at least a page boundary
    }

    uintptr_t alignment_mask = alignment - 1;
    uintptr_t aligned_pointer = (m_free_pointer + alignment_mask) & ~alignment_mask;

    uintptr_t required_size = count * PAGE_SIZE;
    if (aligned_pointer + required_size > m_end_address) {
        return nullptr; // Out of memory
    }

    void* allocated_pages = reinterpret_cast<void*>(aligned_pointer);
    m_free_pointer = aligned_pointer + required_size;
    return allocated_pages;
}
} // namespace allocators
