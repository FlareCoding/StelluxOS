#ifndef PAGE_BOOTSTRAP_ALLOCATOR_H
#define PAGE_BOOTSTRAP_ALLOCATOR_H
#include "phys_frame_allocator.h"

namespace allocators {
class page_bootstrap_allocator : public phys_frame_allocator_impl {
public:
    static page_bootstrap_allocator& get();

    page_bootstrap_allocator() : m_base_address(0), m_free_pointer(0), m_end_address(0) {}
    ~page_bootstrap_allocator() = default;
    
    __PRIVILEGED_CODE void init(uintptr_t base, size_t size);

    __PRIVILEGED_CODE void lock_physical_page(void* paddr) override;
    __PRIVILEGED_CODE void lock_physical_pages(void* paddr, size_t count) override;

    __PRIVILEGED_CODE void free_physical_page(void* paddr) override;
    __PRIVILEGED_CODE void free_physical_pages(void* paddr, size_t count) override;

    __PRIVILEGED_CODE void* alloc_physical_page() override;
    __PRIVILEGED_CODE void* alloc_physical_pages(size_t count) override;
    __PRIVILEGED_CODE void* alloc_physical_pages_aligned(size_t count, uint64_t alignment) override;

private:
    uintptr_t m_base_address; // Start of the memory region
    uintptr_t m_free_pointer; // Pointer to the next free page
    uintptr_t m_end_address;  // End of the memory region
};
} // namespace allocators

#endif // PAGE_BOOTSTRAP_ALLOCATOR_H
