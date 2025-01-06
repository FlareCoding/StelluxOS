#ifndef VMM_H
#define VMM_H
#include <types.h>

// Default flags for privileged kernel pages: Present, writable
#define DEFAULT_PRIV_PAGE_FLAGS 0x3

// Default flags for unprivileged kernel pages: Present, writable, user
#define DEFAULT_UNPRIV_PAGE_FLAGS 0x7

namespace vmm {
/**
 * @brief Allocates a single virtual page and maps it to a new physical page.
 * 
 * @param flags Flags specifying permissions and attributes for the mapping.
 * @return Pointer to the allocated virtual address, or nullptr on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void* alloc_virtual_page(uint64_t flags);

/**
 * @brief Maps an existing physical page to a virtual page.
 * 
 * @param paddr Physical address to map.
 * @param flags Flags specifying permissions and attributes for the mapping.
 * @return Pointer to the virtual address of the mapped page, or nullptr on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void* map_physical_page(uintptr_t paddr, uint64_t flags);

/**
 * @brief Allocates and maps a range of contiguous virtual pages to new physical pages.
 * @note Allocated physical pages are not guaranteed to be contiguous.
 * 
 * @param count Number of pages to allocate.
 * @param flags Flags specifying permissions and attributes for the mapping.
 * @return Pointer to the starting virtual address of the range, or nullptr on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void* alloc_virtual_pages(size_t count, uint64_t flags);

/**
 * @brief Allocates a contiguous range of virtual pages and maps them to contiguous physical pages.
 * 
 * @param count Number of contiguous pages to allocate.
 * @param flags Flags specifying permissions and attributes for the mapping.
 * @return Pointer to the starting virtual address of the allocated range, or nullptr on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void* alloc_contiguous_virtual_pages(size_t count, uint64_t flags);

/**
 * @brief Maps a contiguous range of physical pages to virtual pages.
 * 
 * @param paddr Starting physical address of the contiguous range.
 * @param count Number of contiguous pages to map.
 * @param flags Flags specifying permissions and attributes for the mapping.
 * @return Pointer to the starting virtual address of the mapped range, or nullptr on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void* map_contiguous_physical_pages(uintptr_t paddr, size_t count, uint64_t flags);

/**
 * Allocates a single physical page and provides a linear-mapped virtual address to it.
 * The allocated page is guaranteed to be persistent across all address spaces and
 * accessible via the linear mapping region.
 *
 * @return Linear-mapped virtual address of the allocated page, or nullptr on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void* alloc_linear_mapped_persistent_page();

/**
 * Allocates a contiguous range of physical pages and provides linear-mapped virtual addresses to them.
 * All allocated pages are guaranteed to be persistent across all address spaces and accessible via
 * the linear mapping region. The number of pages to allocate is specified by the `count` parameter.
 *
 * @param flags Flags specifying permissions and attributes for the mapping.
 * @return Linear-mapped virtual address of the first page in the range, or nullptr on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void* alloc_linear_mapped_persistent_pages(size_t count);

/**
 * @brief Unmaps a single virtual page.
 * 
 * @param vaddr Virtual address to unmap.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unmap_virtual_page(uintptr_t vaddr);

/**
 * @brief Unmaps a contiguous range of virtual pages.
 * 
 * @param vaddr Starting virtual address of the range.
 * @param count Number of pages to unmap.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unmap_contiguous_virtual_pages(uintptr_t vaddr, size_t count);
} // namespace vmm

#endif // VMM_H
