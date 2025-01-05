#ifndef PAGE_BITMAP_ALLOCATOR_H
#define PAGE_BITMAP_ALLOCATOR_H
#include "page_frame_allocator.h"
#include <memory/page_bitmap.h>

namespace allocators {
/**
 * @class page_bitmap_allocator
 * @brief Allocates and manages memory pages using a bitmap.
 * 
 * Provides mechanisms for locking, freeing, and allocating memory pages, including support for 
 * aligned allocations and large pages. It utilizes a bitmap to track page usage.
 */
class page_bitmap_allocator : public page_frame_allocator {
public:
    /**
     * @brief Retrieves the singleton instance of the physical page allocator.
     * @return Reference to the singleton instance of the physical `page_bitmap_allocator`.
     * 
     * Provides global access to the allocator for managing physical memory pages.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static page_bitmap_allocator& get_physical_allocator();

    /**
     * @brief Retrieves the singleton instance of the kernel's virtual page allocator.
     * @return Reference to the singleton instance of the virtual `page_bitmap_allocator`.
     * 
     * Provides global access to the kernel's allocator for managing virtual memory pages.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static page_bitmap_allocator& get_virtual_allocator();

    /**
     * @brief Constructs a page bitmap allocator with no base page offset.
     * 
     * Initializes the allocator with an offset of zero for page indices.
     */
    page_bitmap_allocator() : m_base_page_offset(0) {}

    /**
     * @brief Default destructor for the page bitmap allocator.
     */
    ~page_bitmap_allocator() = default;

    /**
     * @brief Initializes the bitmap for page tracking.
     * @param size The size of the bitmap in bits.
     * @param buffer Pointer to the memory buffer backing the bitmap.
     * @param initial_used_value Initial state of the bitmap bits (default: false, meaning all pages are free).
     * 
     * Prepares the bitmap for managing memory pages, setting its size and initial state.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void init_bitmap(uint64_t size, uint8_t* buffer, bool initial_used_value = false);

    /**
     * @brief Sets the base offset for page indices in the allocator.
     * @param offset The base offset to apply to page indices.
     * 
     * Adjusts the allocator to use a custom starting offset for page indices.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void set_base_page_offset(uint64_t offset);

    /**
     * @brief Indicates that the bitmap buffer is a physical address and
     * requires linear kernel virtual-to-physical address translations on access.
     */
    __PRIVILEGED_CODE void mark_bitmap_address_as_physical();

    /**
     * @brief Locks a specific page to prevent its allocation.
     * @param addr Address of the page to lock.
     * 
     * Marks the page as in use, preventing it from being allocated.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void lock_page(void* addr) override;

    /**
     * @brief Locks a range of pages to prevent their allocation.
     * @param addr Address of the first page in the range.
     * @param count Number of pages to lock.
     * 
     * Marks the specified range of pages as in use.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void lock_pages(void* addr, size_t count) override;

    /**
     * @brief Frees a locked page, making it available for allocation.
     * @param addr Address of the page to free.
     * 
     * Marks the page as free in the bitmap.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void free_page(void* addr) override;

    /**
     * @brief Frees a range of locked pages.
     * @param addr Address of the first page in the range.
     * @param count Number of pages to free.
     * 
     * Marks the specified range of pages as free in the bitmap.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void free_pages(void* addr, size_t count) override;

    /**
     * @brief Allocates a single memory page.
     * @return Pointer to the allocated page, or `nullptr` if allocation fails.
     * 
     * Finds and marks a free page as in use.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void* alloc_page() override;

    /**
     * @brief Allocates a range of memory pages.
     * @param count Number of pages to allocate.
     * @return Pointer to the first allocated page, or `nullptr` if allocation fails.
     * 
     * Finds and marks a contiguous range of free pages as in use.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void* alloc_pages(size_t count) override;

    /**
     * @brief Allocates a range of memory pages with a specified alignment.
     * @param count Number of pages to allocate.
     * @param alignment Alignment requirement for the allocated range.
     * @return Pointer to the first allocated page, or `nullptr` if allocation fails.
     * 
     * Finds and marks an aligned range of free pages as in use.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void* alloc_pages_aligned(size_t count, uint64_t alignment) override;

    /**
     * @brief Allocates a single large page.
     * @return Pointer to the allocated large page, or `nullptr` if allocation fails.
     * 
     * Large pages are typically of size 2MB or larger.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void* alloc_large_page();

    /**
     * @brief Allocates a range of large pages.
     * @param count Number of large pages to allocate.
     * @return Pointer to the first allocated large page, or `nullptr` if allocation fails.
     * 
     * Finds and marks a contiguous range of large pages as in use.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void* alloc_large_pages(size_t count);

private:
    paging::page_frame_bitmap m_bitmap; /** Bitmap for tracking page usage */
    uint64_t m_base_page_offset;        /** Base offset for page indices */
};
} // namespace allocators

#endif // PAGE_BITMAP_ALLOCATOR_H
