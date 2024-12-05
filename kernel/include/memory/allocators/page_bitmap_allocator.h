#ifndef PAGE_BITMAP_ALLOCATOR_H
#define PAGE_BITMAP_ALLOCATOR_H
#include "phys_frame_allocator.h"

namespace allocators {
class page_bitmap_allocator : public phys_frame_allocator {
public:
    static page_bitmap_allocator& get();

    page_bitmap_allocator() = default;
    ~page_bitmap_allocator() = default;

    __PRIVILEGED_CODE void lock_physical_page(void* paddr) override;
    __PRIVILEGED_CODE void lock_physical_pages(void* paddr, size_t count) override;

    __PRIVILEGED_CODE void free_physical_page(void* paddr) override;
    __PRIVILEGED_CODE void free_physical_pages(void* paddr, size_t count) override;

    __PRIVILEGED_CODE void* alloc_physical_page() override;
    __PRIVILEGED_CODE void* alloc_physical_pages(size_t count) override;
    __PRIVILEGED_CODE void* alloc_physical_pages_aligned(size_t count, uint64_t alignment) override;
};
} // namespace allocators

#endif // PAGE_BITMAP_ALLOCATOR_H
