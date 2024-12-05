#ifndef PHYS_FRAME_ALLOCATOR_H
#define PHYS_FRAME_ALLOCATOR_H
#include <types.h>

namespace allocators {
class phys_frame_allocator_impl {
public:
    phys_frame_allocator_impl() = default;
    virtual ~phys_frame_allocator_impl() = default;

    __PRIVILEGED_CODE virtual void lock_physical_page(void* paddr) = 0;
    __PRIVILEGED_CODE virtual void lock_physical_pages(void* paddr, size_t count) = 0;

    __PRIVILEGED_CODE virtual void free_physical_page(void* paddr) = 0;
    __PRIVILEGED_CODE virtual void free_physical_pages(void* paddr, size_t count) = 0;

    __PRIVILEGED_CODE virtual void* alloc_physical_page() = 0;
    __PRIVILEGED_CODE virtual void* alloc_physical_pages(size_t count) = 0;
    __PRIVILEGED_CODE virtual void* alloc_physical_pages_aligned(size_t count, uint64_t alignment) = 0;
};
} // namespace allocators

#endif // PHYS_FRAME_ALLOCATOR_H
