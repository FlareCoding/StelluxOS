#ifndef PAGING_H
#define PAGING_H
#include <types.h>

#define PAGE_SIZE 0x1000

#define PAGE_TABLE_ENTRIES 512

#define USER_PAGE    1
#define KERNEL_PAGE  0

#define PAGE_ATTRIB_CACHE_DISABLED 0x01  // Bit 0
#define PAGE_ATTRIB_WRITE_THROUGH  0x02  // Bit 1
#define PAGE_ATTRIB_ACCESS_TYPE    0x04  // Bit 2

// Flags for page table entries
#define PTE_PRESENT       0x1
#define PTE_RW            0x2
#define PTE_US            0x4
#define PTE_PWT           0x8
#define PTE_PCD           0x10
#define PTE_ACCESSED      0x20
#define PTE_DIRTY         0x40
#define PTE_PAT           0x80
#define PTE_GLOBAL        0x100
#define PTE_NX            (1ULL << 63)

namespace paging {
typedef struct page_table_entry {
    union
    {
        struct
        {
            uint64_t present              : 1;    // Must be 1, region invalid if 0.
            uint64_t read_write           : 1;    // If 0, writes not allowed.
            uint64_t user_supervisor      : 1;    // If 0, user-mode accesses not allowed.
            uint64_t page_write_through   : 1;    // Determines the memory type used to access the memory.
            uint64_t page_cache_disabled  : 1;    // Determines the memory type used to access the memory.
            uint64_t accessed             : 1;    // If 0, this entry has not been used for translation.
            uint64_t dirty                : 1;    // If 0, the memory backing this page has not been written to.
            uint64_t page_access_type     : 1;    // Determines the memory type used to access the memory.
            uint64_t global               : 1;    // If 1 and the PGE bit of CR4 is set, translations are global.
            uint64_t ignored2             : 3;
            uint64_t page_frame_number    : 36;   // The page frame number of the backing physical page.
            uint64_t reserved             : 4;
            uint64_t ignored3             : 7;
            uint64_t protection_key       : 4;    // If the PKE bit of CR4 is set, determines the protection key.
            uint64_t execute_disable      : 1;    // If 1, instruction fetches not allowed.
        } __attribute__((packed));
        uint64_t value;
    };
} __attribute__((packed)) pte_t;
static_assert(sizeof(page_table_entry) == 8);

typedef struct page_directory_entry {
    union
    {
        struct
        {
            uint64_t present              : 1;    // [0] P: Must be 1 if the entry is valid.
            uint64_t read_write           : 1;    // [1] R/W: 0 = read-only, 1 = read/write.
            uint64_t user_supervisor      : 1;    // [2] U/S: 0 = supervisor, 1 = user.
            uint64_t page_write_through   : 1;    // [3] PWT: Determines write-through caching.
            uint64_t page_cache_disabled  : 1;    // [4] PCD: Disables caching for this page.
            uint64_t accessed             : 1;    // [5] A: Set by hardware when accessed.
            uint64_t dirty                : 1;    // [6] D: Set by hardware when written to.
            uint64_t page_size            : 1;    // [7] PS: If 1, maps a 2MB page; else points to a PT.
            uint64_t global               : 1;    // [8] G: Global page if CR4.PGE is set.
            uint64_t ignored1             : 3;    // [9–11] Ignored by hardware.
            uint64_t page_frame_number    : 36;   // [12–51] Physical address of 2MB page or next-level PT.
            uint64_t reserved             : 4;    // [52–55] Reserved for future use.
            uint64_t ignored2             : 7;    // [56–62] Ignored by hardware.
            uint64_t execute_disable      : 1;    // [63] XD: Instruction fetch disallowed if set.
        };
        uint64_t value;                          // Complete 64-bit entry.
    };
} __attribute__((packed)) pde_t;
static_assert(sizeof(page_directory_entry) == 8);

struct page_table {
    pte_t entries[PAGE_TABLE_ENTRIES];
} __attribute__((aligned(PAGE_SIZE)));

struct virt_addr_indices_t {
    uint16_t pml4;
    uint16_t pdpt;
    uint16_t pdt;
    uint16_t pt;
};

virt_addr_indices_t get_vaddr_page_table_indices(uint64_t virt_addr);

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

class bootstrap_allocator : public phys_frame_allocator_impl {
public:
    bootstrap_allocator() : m_base_address(0), m_free_pointer(0), m_end_address(0) {}
    ~bootstrap_allocator() = default;
    
    __PRIVILEGED_CODE void init(uintptr_t base, size_t size);

    __PRIVILEGED_CODE void lock_physical_page(void* paddr) override;
    __PRIVILEGED_CODE void lock_physical_pages(void* paddr, size_t count) override;

    __PRIVILEGED_CODE void free_physical_page(void* paddr) override;
    __PRIVILEGED_CODE void free_physical_pages(void* paddr, size_t count) override;

    __PRIVILEGED_CODE void* alloc_physical_page() override;
    __PRIVILEGED_CODE void* alloc_physical_pages(size_t count) override;
    __PRIVILEGED_CODE void* alloc_physical_pages_aligned(size_t count, uint64_t alignment) override;

private:
    uintptr_t m_base_address; // Start of the memory region
    uintptr_t m_free_pointer; // Pointer to the next free page
    uintptr_t m_end_address;  // End of the memory region
};

class bitmap_allocator : public phys_frame_allocator_impl {
public:
    bitmap_allocator() = default;
    ~bitmap_allocator() = default;

    __PRIVILEGED_CODE void lock_physical_page(void* paddr) override;
    __PRIVILEGED_CODE void lock_physical_pages(void* paddr, size_t count) override;

    __PRIVILEGED_CODE void free_physical_page(void* paddr) override;
    __PRIVILEGED_CODE void free_physical_pages(void* paddr, size_t count) override;

    __PRIVILEGED_CODE void* alloc_physical_page() override;
    __PRIVILEGED_CODE void* alloc_physical_pages(size_t count) override;
    __PRIVILEGED_CODE void* alloc_physical_pages_aligned(size_t count, uint64_t alignment) override;
};
} // namespace allocators

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

