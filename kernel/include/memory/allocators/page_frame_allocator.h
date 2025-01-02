#ifndef PAGE_FRAME_ALLOCATOR_H
#define PAGE_FRAME_ALLOCATOR_H
#include <types.h>

namespace allocators {
/**
 * @class page_frame_allocator
 * @brief Abstract base class for managing page frame allocation.
 * 
 * Defines a virtual interface for locking, freeing, and allocating memory pages. Derived classes
 * implement specific allocation strategies based on this interface.
 */
class page_frame_allocator {
public:
    /**
     * @brief Default constructor for the page frame allocator.
     */
    page_frame_allocator() = default;

    /**
     * @brief Virtual destructor for the page frame allocator.
     * 
     * Ensures proper cleanup in derived classes.
     */
    virtual ~page_frame_allocator() = default;

    /**
     * @brief Locks a specific page to prevent its allocation.
     * @param addr Address of the page to lock.
     * 
     * Marks the page as in use, preventing it from being allocated.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void lock_page(void* addr) = 0;

    /**
     * @brief Locks a range of pages to prevent their allocation.
     * @param addr Address of the first page in the range.
     * @param count Number of pages to lock.
     * 
     * Marks the specified range of pages as in use.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void lock_pages(void* addr, size_t count) = 0;

    /**
     * @brief Frees a previously locked page, making it available for allocation.
     * @param addr Address of the page to free.
     * 
     * Marks the page as free, allowing it to be allocated again.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void free_page(void* addr) = 0;

    /**
     * @brief Frees a range of locked pages.
     * @param addr Address of the first page in the range.
     * @param count Number of pages to free.
     * 
     * Marks the specified range of pages as free, making them available for future allocations.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void free_pages(void* addr, size_t count) = 0;

    /**
     * @brief Allocates a single memory page.
     * @return Pointer to the allocated page, or `nullptr` if allocation fails.
     * 
     * Finds and marks a free page as in use.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void* alloc_page() = 0;

    /**
     * @brief Allocates a range of memory pages.
     * @param count Number of pages to allocate.
     * @return Pointer to the first allocated page, or `nullptr` if allocation fails.
     * 
     * Finds and marks a contiguous range of free pages as in use.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void* alloc_pages(size_t count) = 0;

    /**
     * @brief Allocates a range of memory pages with a specified alignment.
     * @param count Number of pages to allocate.
     * @param alignment Alignment requirement for the allocated range.
     * @return Pointer to the first allocated page, or `nullptr` if allocation fails.
     * 
     * Ensures the allocated range starts at an address aligned to the specified boundary.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void* alloc_pages_aligned(size_t count, uint64_t alignment) = 0;
};
} // namespace allocators

#endif // PAGE_FRAME_ALLOCATOR_H
