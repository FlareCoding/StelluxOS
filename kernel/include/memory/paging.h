#ifndef PAGING_H
#define PAGING_H
#include <types.h>

#define PAGE_SIZE 0x1000

namespace paging {
/**
 * @class page_frame_bitmap
 * @brief Manages a bitmap for physical page allocation in the OS kernel.
 * 
 * The `page_frame_bitmap` class provides functionalities to initialize and manipulate
 * a bitmap that tracks the usage of physical memory pages. It allows marking pages as
 * free or used, checking their status, and managing multiple pages at once. This is
 * essential for efficient memory management within the kernel.
 */
class page_frame_bitmap {
public:
    /**
     * @brief Retrieves the singleton instance of the page_frame_bitmap.
     * 
     * This static method ensures that only one instance of the `page_frame_bitmap`
     * exists throughout the system. It provides global access to the bitmap for
     * managing physical memory pages.
     * 
     * @return page_frame_bitmap& Reference to the singleton instance of the bitmap.
     */
    __PRIVILEGED_CODE static page_frame_bitmap& get();

    /**
     * @brief Default constructor for the page_frame_bitmap class.
     * 
     * Initializes a new instance of the `page_frame_bitmap` class. The constructor
     * does not perform any initialization of the bitmap itself, this should be done
     * using the `init` method after construction.
     */
    __PRIVILEGED_CODE page_frame_bitmap() = default;
    
    /**
     * @brief Initializes the bitmap with a specified size and buffer.
     * 
     * This method sets up the bitmap by defining its size (in pages) and assigning
     * the memory buffer that will store the bitmap data. It must be called before
     * any operations are performed on the bitmap. All the pages are initially
     * marked as 'free'.
     * 
     * @param size The total number of pages that the bitmap will manage.
     * @param buffer Pointer to the memory buffer that will hold the bitmap data.
     */
    __PRIVILEGED_CODE void init(uint64_t size, uint8_t* buffer);

    /**
     * @brief Retrieves the size of the bitmap.
     * 
     * This method returns the total number of pages that the bitmap is configured
     * to manage. It provides a way to query the capacity of the bitmap.
     * 
     * @return uint64_t The total number of pages tracked by the bitmap.
     */
    __PRIVILEGED_CODE uint64_t get_size() const;

    /**
     * @brief Retrieves the index of the next available free page.
     * 
     * Note: the returned index has a high chance of being a free page,
     *       but is not guaranteed as the index gets updated to the next
     *       contiguous page during allocation routines, and the next
     *       page is not always guaranteed to be free.
     * 
     * @return uint64_t The total number of pages tracked by the bitmap.
     */
    __PRIVILEGED_CODE uint64_t get_next_free_index() const;

    /**
     * @brief Sets the next free page index in the bitmap.
     * 
     * This method allows for external access to setting the next
     * available free page index in the bitmap.
     * 
     * @return uint64_t The total number of pages tracked by the bitmap.
     */
    __PRIVILEGED_CODE void set_next_free_index(uint64_t idx) const;

    /**
     * @brief Marks a single physical page as free.
     * 
     * This method updates the bitmap to indicate that the specified physical page
     * is now free and available for allocation. It is used when a page is released
     * back to the memory pool.
     * 
     * @param paddr The physical address of the page to mark as free.
     * @return true If the page was successfully marked as free.
     * @return false If the operation failed (e.g., invalid address).
     */
    __PRIVILEGED_CODE bool mark_page_free(void* paddr);

    /**
     * @brief Marks a single physical page as used.
     * 
     * This method updates the bitmap to indicate that the specified physical page
     * is now in use and should not be allocated again until it is freed. It is used
     * when a page is allocated for use by the system or applications.
     * 
     * @param paddr The physical address of the page to mark as used.
     * @return true If the page was successfully marked as used.
     * @return false If the operation failed (e.g., invalid address).
     */
    __PRIVILEGED_CODE bool mark_page_used(void* paddr);

    /**
     * @brief Marks multiple physical pages as free.
     * 
     * This method updates the bitmap to indicate that a range of physical pages
     * starting from the specified address are now free and available for allocation.
     * It is used when multiple contiguous pages are released back to the memory pool.
     * 
     * @param paddr Pointer to the starting physical address of the pages to mark as free.
     * @param count The number of contiguous pages to mark as free.
     * @return true If all specified pages were successfully marked as free.
     * @return false If the operation failed (e.g., invalid address range).
     */
    __PRIVILEGED_CODE bool mark_pages_free(void* paddr, size_t count);
    
    /**
     * @brief Marks multiple physical pages as used.
     * 
     * This method updates the bitmap to indicate that a range of physical pages
     * starting from the specified address are now in use and should not be allocated
     * again until they are freed. It is used when multiple contiguous pages are
     * allocated for system or application use.
     * 
     * @param paddr Pointer to the starting physical address of the pages to mark as used.
     * @param count The number of contiguous pages to mark as used.
     * @return true If all specified pages were successfully marked as used.
     * @return false If the operation failed (e.g., invalid address range).
     */
    __PRIVILEGED_CODE bool mark_pages_used(void* paddr, size_t count);

    /**
     * @brief Checks if a single physical page is free.
     * 
     * This method queries the bitmap to determine whether the specified physical page
     * is currently marked as free. It is useful for verifying the availability of a page
     * before attempting to allocate it.
     * 
     * @param paddr The physical address of the page to check.
     * @return true If the page is marked as free.
     * @return false If the page is marked as used or the address is invalid.
     */
    __PRIVILEGED_CODE bool is_page_free(void* paddr);
    
    /**
     * @brief Checks if a single physical page is used.
     * 
     * This method queries the bitmap to determine whether the specified physical page
     * is currently marked as used. It is useful for verifying the allocation status of a page.
     * 
     * @param paddr The physical address of the page to check.
     * @return true If the page is marked as used.
     * @return false If the page is marked as free or the address is invalid.
     */
    __PRIVILEGED_CODE bool is_page_used(void* paddr);

private:
    /**
     * @brief Sets the value of a single physical page in the bitmap.
     * 
     * This private helper method updates the bitmap to reflect the usage status of a
     * single physical page. It abstracts the underlying bitmap manipulation logic.
     * 
     * @param paddr The physical address of the page to set.
     * @param value The value to set for the page (true for used, false for free).
     * @return true If the page value was successfully set.
     * @return false If the operation failed (e.g., invalid address).
     */
    __PRIVILEGED_CODE bool _set_page_value(void* paddr, bool value);
    
    /**
     * @brief Retrieves the usage status of a single physical page from the bitmap.
     * 
     * This private helper method queries the bitmap to determine whether a specific
     * physical page is marked as used or free. It abstracts the underlying bitmap access logic.
     * 
     * @param paddr The physical address of the page to query.
     * @return true If the page is marked as used.
     * @return false If the page is marked as free or the address is invalid.
     */
    __PRIVILEGED_CODE bool _get_page_value(void* paddr);

    /**
     * @brief Calculates the bitmap index for a given physical address.
     * 
     * This private helper method converts a physical address to its corresponding index
     * within the bitmap. It is used internally to map physical pages to their bitmap entries.
     * 
     * @param paddr The physical address of the page.
     * @return uint64_t The index within the bitmap that corresponds to the physical address.
     */
    __PRIVILEGED_CODE uint64_t _get_addr_index(void* paddr);

    /**
     * @brief The total number of pages managed by the bitmap.
     * 
     * This private member variable stores the size of the bitmap, representing the
     * total number of physical pages that are tracked for allocation and deallocation.
     */
    __PRIVILEGED_DATA static uint64_t _size;

    /**
     * @brief Pointer to the memory buffer that holds the bitmap data.
     * 
     * This private member variable points to the buffer in memory where the bitmap
     * is stored. Each bit in the buffer represents the status (free or used) of a
     * corresponding physical memory page.
     */
    __PRIVILEGED_DATA static uint8_t* _buffer;

    /**
     * @brief Index of the next available free physical frame in the bitmap.
     * 
     * Having a way to keep track of the next free physical page is a way to
     * optimize physical frame allocation by avoiding redundant linear searching.
     */
    __PRIVILEGED_DATA static uint64_t _next_free_index;
};

/**
 * @brief Locks a single physical page to prevent it from being allocated.
 * 
 * This function marks the specified physical page as locked, ensuring that it remains
 * reserved and is not available for allocation to other processes or components.
 * 
 * @param paddr The physical address of the page to lock.
 */
__PRIVILEGED_CODE void lock_physical_page(void* paddr);

/**
 * @brief Locks multiple contiguous physical pages to prevent them from being allocated.
 * 
 * This function marks a range of physical pages starting from the specified address as locked,
 * ensuring that these pages remain reserved and are not available for allocation to other
 * processes or components. Locking multiple pages is useful for reserving larger memory blocks
 * required for specific system operations or hardware interactions.
 * 
 * @param paddr Pointer to the starting physical address of the pages to lock.
 * @param count The number of contiguous pages to lock.
 */
__PRIVILEGED_CODE void lock_physical_pages(void* paddr, size_t count);

/**
 * @brief Frees a single physical page, making it available for allocation.
 * 
 * This function marks the specified physical page as free, allowing it to be allocated
 * to processes or components that request memory.
 * 
 * @param paddr The physical address of the page to free.
 */
__PRIVILEGED_CODE void free_physical_page(void* paddr);

/**
 * @brief Frees multiple contiguous physical pages, making them available for allocation.
 * 
 * This function marks a range of physical pages starting from the specified address as free,
 * allowing them to be allocated to processes or components that request memory.
 * 
 * @param paddr Pointer to the starting physical address of the pages to free.
 * @param count The number of contiguous pages to free.
 */
__PRIVILEGED_CODE void free_physical_pages(void* paddr, size_t count);

/**
 * @brief Allocates a single free physical page.
 * 
 * This function searches the physical page bitmap for a free page, marks it as used,
 * and returns its physical address.
 * 
 * @return void* The physical address of the allocated page, or nullptr if no free pages are available.
 */
__PRIVILEGED_CODE void* alloc_physical_page();

/**
 * @brief Allocates multiple contiguous free physical pages.
 * 
 * This function searches the physical page bitmap for a contiguous block of free pages,
 * marks them as used, and returns the starting physical address of the allocated block.
 * 
 * @param count The number of contiguous pages to allocate.
 * @return void* The starting physical address of the allocated pages, or nullptr if no suitable
 *               contiguous block of free pages is available.
 */
__PRIVILEGED_CODE void* alloc_physical_pages(size_t count);

/**
 * @brief Allocates multiple contiguous free physical pages with a specified alignment.
 * 
 * This function searches the physical page bitmap for a contiguous block of free pages that
 * satisfies the specified alignment requirement. It marks the allocated pages as used and
 * returns the starting physical address of the allocated block. Alignment is crucial for
 * certain hardware operations or memory regions that require specific boundary constraints.
 * 
 * @param count The number of contiguous pages to allocate.
 * @param alignment The required alignment for the allocated block in bytes. Must be a power of two.
 * @return void* The starting physical address of the allocated and aligned pages, or nullptr if
 *               no suitable contiguous and aligned block of free pages is available.
 */
__PRIVILEGED_CODE void* alloc_physical_pages_aligned(size_t count, uint64_t alignment);

/**
 * @brief Initializes the physical memory allocator using the Multiboot EFI memory map.
 * 
 * This function sets up the bitmap-based physical page allocator by parsing the EFI memory map
 * provided through the Multiboot Information (MBI) structure. It configures the allocator to manage
 * available and reserved physical memory regions by marking pages as free or used based on the
 * memory map information. Proper initialization of the physical allocator is essential for enabling
 * dynamic memory allocation.
 * 
 * @param mbi_efi_mmap_tag Pointer to the EFI memory map tag within the Multiboot Information structure.
 *                         This tag contains detailed information about the memory regions, including
 *                         their physical addresses, sizes, and types (e.g., usable, reserved). The
 *                         allocator uses this data to determine which pages are available for
 *                         allocation and which should remain reserved.
 */
__PRIVILEGED_CODE void init_physical_allocator(void* mbi_efi_mmap_tag);
} // namespace paging

#endif // PAGING_H

