#ifndef PAGE_BITMAP_ALLOCATOR_H
#define PAGE_BITMAP_ALLOCATOR_H
#include "page_frame_allocator.h"
#include <memory/page_bitmap.h>

namespace allocators {
class page_bitmap_allocator : public page_frame_allocator {
public:
    __PRIVILEGED_CODE static page_bitmap_allocator& get_physical_allocator();
    __PRIVILEGED_CODE static page_bitmap_allocator& get_virtual_allocator();

    page_bitmap_allocator() : m_base_page_offset(0) {}
    ~page_bitmap_allocator() = default;

    __PRIVILEGED_CODE void init_bitmap(uint64_t size, uint8_t* buffer, bool initial_used_value = false);
    __PRIVILEGED_CODE void set_base_page_offset(uint64_t offset);

    __PRIVILEGED_CODE void lock_page(void* addr) override;
    __PRIVILEGED_CODE void lock_pages(void* addr, size_t count) override;

    __PRIVILEGED_CODE void free_page(void* addr) override;
    __PRIVILEGED_CODE void free_pages(void* addr, size_t count) override;

    __PRIVILEGED_CODE void* alloc_page() override;
    __PRIVILEGED_CODE void* alloc_pages(size_t count) override;
    __PRIVILEGED_CODE void* alloc_pages_aligned(size_t count, uint64_t alignment) override;

    __PRIVILEGED_CODE void* alloc_large_page();
    __PRIVILEGED_CODE void* alloc_large_pages(size_t count);

private:
    paging::page_frame_bitmap m_bitmap;
    uint64_t m_base_page_offset;
};
} // namespace allocators

#endif // PAGE_BITMAP_ALLOCATOR_H
