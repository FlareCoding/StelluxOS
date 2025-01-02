#ifndef PAGE_BOOTSTRAP_ALLOCATOR_H
#define PAGE_BOOTSTRAP_ALLOCATOR_H
#include "page_frame_allocator.h"

namespace allocators {
/**
 * @class page_bootstrap_allocator
 * @brief Provides basic page allocation during system bootstrap.
 * 
 * The `page_bootstrap_allocator` is a simple allocator designed to manage memory pages during
 * early system initialization. It operates over a fixed memory region and does not track free or 
 * used pages beyond simple pointer manipulation.
 */
class page_bootstrap_allocator : public page_frame_allocator {
public:
    /**
     * @brief Retrieves the singleton instance of the bootstrap page allocator.
     * @return Reference to the singleton instance of the `page_bootstrap_allocator`.
     * 
     * Provides global access to the bootstrap allocator for early memory management.
     */
    static page_bootstrap_allocator& get();

    /**
     * @brief Constructs a page bootstrap allocator with default parameters.
     * 
     * Initializes the allocator with zeroed base, free pointer, and end address.
     */
    page_bootstrap_allocator() : m_base_address(0), m_free_pointer(0), m_end_address(0) {}

    /**
     * @brief Default destructor for the page bootstrap allocator.
     */
    ~page_bootstrap_allocator() = default;

    /**
     * @brief Initializes the bootstrap allocator with a fixed memory region.
     * @param base The starting address of the memory region.
     * @param size The size of the memory region in bytes.
     * 
     * Sets up the allocator to manage a fixed range of memory pages for allocation.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void init(uintptr_t base, size_t size);

    /**
     * @brief Locks a specific page to prevent its allocation.
     * @param addr Address of the page to lock.
     * 
     * In the bootstrap allocator, this function is a no-op as it does not track page usage.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void lock_page(void* addr) override;

    /**
     * @brief Locks a range of pages to prevent their allocation.
     * @param addr Address of the first page in the range.
     * @param count Number of pages to lock.
     * 
     * In the bootstrap allocator, this function is a no-op as it does not track page usage.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void lock_pages(void* addr, size_t count) override;

    /**
     * @brief Frees a previously allocated page.
     * @param addr Address of the page to free.
     * 
     * In the bootstrap allocator, this function is a no-op as it does not support freeing pages.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void free_page(void* addr) override;

    /**
     * @brief Frees a range of pages.
     * @param addr Address of the first page in the range.
     * @param count Number of pages to free.
     * 
     * In the bootstrap allocator, this function is a no-op as it does not support freeing pages.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void free_pages(void* addr, size_t count) override;

    /**
     * @brief Allocates a single memory page.
     * @return Pointer to the allocated page, or `nullptr` if allocation fails.
     * 
     * Allocates the next available page from the memory region, moving the free pointer forward.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void* alloc_page() override;

    /**
     * @brief Allocates a range of memory pages.
     * @param count Number of pages to allocate.
     * @return Pointer to the first allocated page, or `nullptr` if allocation fails.
     * 
     * Allocates a contiguous range of pages, updating the free pointer accordingly.
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
     * Allocates pages starting at an aligned address, ensuring the alignment requirement is met.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void* alloc_pages_aligned(size_t count, uint64_t alignment) override;

private:
    uintptr_t m_base_address; /** Starting address of the managed memory region */
    uintptr_t m_free_pointer; /** Pointer to the next free page */
    uintptr_t m_end_address;  /** Ending address of the managed memory region */
};
} // namespace allocators

#endif // PAGE_BOOTSTRAP_ALLOCATOR_H
