#ifndef PAGE_FRAME_ALLOCATOR_H
#define PAGE_FRAME_ALLOCATOR_H
#include <types.h>

namespace allocators {
class page_frame_allocator {
public:
    page_frame_allocator() = default;
    virtual ~page_frame_allocator() = default;

    __PRIVILEGED_CODE virtual void lock_page(void* addr) = 0;
    __PRIVILEGED_CODE virtual void lock_pages(void* addr, size_t count) = 0;

    __PRIVILEGED_CODE virtual void free_page(void* addr) = 0;
    __PRIVILEGED_CODE virtual void free_pages(void* addr, size_t count) = 0;

    __PRIVILEGED_CODE virtual void* alloc_page() = 0;
    __PRIVILEGED_CODE virtual void* alloc_pages(size_t count) = 0;
    __PRIVILEGED_CODE virtual void* alloc_pages_aligned(size_t count, uint64_t alignment) = 0;
};
} // namespace allocators

#endif // PAGE_FRAME_ALLOCATOR_H
