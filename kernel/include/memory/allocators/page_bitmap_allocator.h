#ifndef PAGE_BITMAP_ALLOCATOR_H
#define PAGE_BITMAP_ALLOCATOR_H
#include "page_frame_allocator.h"
#include <memory/page_bitmap.h>

namespace allocators {
class page_bitmap_allocator : public page_frame_allocator {
public:
    __PRIVILEGED_CODE static page_bitmap_allocator& get_physical_allocator();
    __PRIVILEGED_CODE static page_bitmap_allocator& get_virtual_allocator();

    page_bitmap_allocator() = default;
    ~page_bitmap_allocator() = default;

    __PRIVILEGED_CODE void init_bitmap(uint64_t size, uint8_t* buffer, bool initial_used_value = false);

    __PRIVILEGED_CODE void lock_page(void* addr) override;
    __PRIVILEGED_CODE void lock_pages(void* addr, size_t count) override;

    __PRIVILEGED_CODE void free_page(void* addr) override;
    __PRIVILEGED_CODE void free_pages(void* addr, size_t count) override;

    __PRIVILEGED_CODE void* alloc_page() override;
    __PRIVILEGED_CODE void* alloc_pages(size_t count) override;
    __PRIVILEGED_CODE void* alloc_pages_aligned(size_t count, uint64_t alignment) override;

private:
    paging::page_frame_bitmap m_bitmap;
};
} // namespace allocators

#endif // PAGE_BITMAP_ALLOCATOR_H
