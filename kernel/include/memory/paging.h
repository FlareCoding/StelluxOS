#ifndef PAGING_H
#define PAGING_H
#include "allocators/page_bitmap_allocator.h"

#define PAGE_SIZE           0x1000
#define LARGE_PAGE_SIZE     (2 * 1024 * 1024)
#define PAGE_ALIGN(value)   (((value) + (PAGE_SIZE) - 1) & ~((PAGE_SIZE) - 1))
#define PAGE_ALIGN_UP(value) PAGE_ALIGN(value)
#define PAGE_ALIGN_DOWN(value) ((value) & ~((PAGE_SIZE) - 1))

#define PAGE_TABLE_ENTRIES 512

#define PTE_PRESENT       (1ULL << 0)   // Page is present
#define PTE_RW            (1ULL << 1)   // Read/Write: 1=Writable, 0=Read-only
#define PTE_US            (1ULL << 2)   // User/Supervisor: 1=User, 0=Supervisor
#define PTE_PWT           (1ULL << 3)   // Page-Level Write-Through
#define PTE_PCD           (1ULL << 4)   // Page-Level Cache Disable
#define PTE_ACCESSED      (1ULL << 5)   // Accessed
#define PTE_DIRTY         (1ULL << 6)   // Dirty
#define PTE_PAT           (1ULL << 7)   // Page Attribute Table
#define PTE_PS            (1ULL << 7)   // Page Size (1=Large Page, 0=4KB Page)
#define PTE_GLOBAL        (1ULL << 8)   // Global Page: Ignored in CR4.PGE=0
#define PTE_NX            (1ULL << 63)  // No-Execute: Only valid if EFER.NXE=1

// Custom flags for kernel/user pages
#define PTE_KERNEL_PAGE   (0ULL)        // Kernel page (Supervisor, no additional flags)
#define PTE_USER_PAGE     (PTE_US)      // User-accessible page

// Default flags for privileged kernel pages: Present, writable
#define PTE_DEFAULT_PRIV_KERNEL_FLAGS (PTE_PRESENT | PTE_RW)

// Default flags for unprivileged kernel pages: Present, writable, user
#define PTE_DEFAULT_UNPRIV_KERNEL_FLAGS (PTE_PRESENT | PTE_RW | PTE_US)

// Macros for page frame number (PFN) calculations
#define ADDR_TO_PFN(addr) ((addr) >> 12)  // Convert address to page frame number
#define PFN_TO_ADDR(pfn) ((pfn) << 12)    // Convert page frame number to address

// Base address of the kernel virtual address space
#define KERN_VIRT_BASE 0xffffff8000000000

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

/**
 * Converts a physical address to a virtual address in the linear mapping region.
 * If linear mapping is not initialized, returns the original physical address.
 *
 * @param paddr The physical address to convert.
 * @return The corresponding virtual address in the linear mapping region, or the original
 *         address if linear mapping is not initialized.
 */
void* phys_to_virt_linear(uintptr_t paddr);
void* phys_to_virt_linear(void* paddr);

/**
 * Converts a virtual address in the linear mapping region back to its physical address.
 * If linear mapping is not initialized, returns the original virtual address.
 *
 * @param vaddr The virtual address to convert.
 * @return The corresponding physical address, or the original address if linear mapping
 *         is not initialized.
 */
uintptr_t virt_to_phys_linear(void* vaddr);
uintptr_t virt_to_phys_linear(uintptr_t vaddr);

/**
 * @brief Retrieves the page table indices for a given virtual address.
 * 
 * This function decomposes a 64-bit virtual address into its constituent indices used
 * for traversing the multi-level page tables (e.g., PML4, PDPT, PD, PT) in a typical
 * x86_64 paging structure. These indices are essential for locating the appropriate
 * page table entries that map the virtual address to a physical address.
 * 
 * @param vaddr The 64-bit virtual address for which to retrieve the page table indices.
 * 
 * @return virt_addr_indices_t A structure containing the individual indices for each level
 *                             of the page table hierarchy corresponding to the provided virtual address.
 * 
 * @note Privilege: **required**
 */
virt_addr_indices_t get_vaddr_page_table_indices(uint64_t vaddr);

/**
 * @brief Retrieves the current PML4 table by reading the CR3 register.
 * 
 * This function reads the CR3 register, which contains the physical address of the current
 * PML4 (Page Map Level 4) table, and casts it to a pointer to `page_table`.
 * 
 * @return A physical address pointer to the current PML4 `page_table`.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE page_table* get_pml4();

/**
 * @brief Sets the current PML4 table by writing to the CR3 register.
 * 
 * This function takes a pointer to a `page_table` (PML4) and writes its
 * address to the CR3 register.
 * 
 * @param pml4 A physical address pointer to the `page_table` to be set as the new PML4.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_pml4(page_table* pml4);

/**
 * @brief Maps a virtual address to a physical address in the specified page table.
 * 
 * This function creates a mapping between the provided virtual address (`vaddr`) and
 * physical address (`paddr`) within the given page table (`pml4`). The `flags` parameter
 * defines the access permissions and attributes for the mapping. An optional physical
 * frame allocator can be supplied; if not, the default page bitmap allocator is used.
 * 
 * @param vaddr The virtual address to be mapped.
 * @param paddr The physical address to map to the virtual address.
 * @param flags Flags specifying the permissions and attributes for the mapping.
 * @param pml4 Pointer to the PML4 (top-level) page table where the mapping will be added.
 * @param allocator Reference to a physical frame allocator. Defaults to the page bitmap allocator.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void map_page(
    uintptr_t vaddr,
    uintptr_t paddr,
    uint64_t flags,
    page_table* pml4,
    allocators::page_frame_allocator& allocator =
        allocators::page_bitmap_allocator::get_physical_allocator()
);

/**
 * @brief Maps a contiguous range of virtual addresses to a contiguous range of physical addresses.
 * 
 * This function maps multiple contiguous pages starting from `vaddr` to `paddr`.
 * The number of pages to map is specified by `num_pages`. All pages in the range
 * will share the same flags.
 * 
 * @param vaddr The starting virtual address of the range.
 * @param paddr The starting physical address of the range.
 * @param num_pages The number of pages to map.
 * @param flags The flags specifying permissions and attributes for the mapping.
 * @param pml4 Pointer to the PML4 (top-level) page table.
 * @param allocator Reference to a physical frame allocator. Defaults to the page bitmap allocator.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void map_pages(
    uintptr_t vaddr,
    uintptr_t paddr,
    size_t num_pages,
    uint64_t flags,
    page_table* pml4,
    allocators::page_frame_allocator& allocator =
        allocators::page_bitmap_allocator::get_physical_allocator()
);

/**
 * @brief Maps a virtual address to a large physical page address in the specified page table.
 * 
 * Maps a 2MB large page, eliminating the need for the bottom level page table.
 * 
 * @param vaddr The starting virtual address of the range.
 * @param paddr The starting physical address of the range.
 * @param flags The flags specifying permissions and attributes for the mapping.
 * @param pml4 Pointer to the PML4 (top-level) page table.
 * @param allocator Reference to a physical frame allocator. Defaults to the page bitmap allocator.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void map_large_page(
    uintptr_t vaddr,
    uintptr_t paddr,
    uint64_t flags,
    page_table* pml4,
    allocators::page_frame_allocator& allocator =
        allocators::page_bitmap_allocator::get_physical_allocator()
); 

/**
 * @brief Retrieves the PML4 (Page Map Level 4) entry for a given virtual address.
 * 
 * @param vaddr The virtual address.
 * @return pde_t* Pointer to the PML4 entry. Returns nullptr if the entry is not present.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE pde_t* get_pml4_entry(void* vaddr);

/**
 * @brief Retrieves the PDPT (Page Directory Pointer Table) entry for a given virtual address.
 * 
 * @param vaddr The virtual address.
 * @return pde_t* Pointer to the PDPT entry. Returns nullptr if the entry is not present.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE pde_t* get_pdpt_entry(void* vaddr);

/**
 * @brief Retrieves the PDT (Page Directory Table) entry for a given virtual address.
 * 
 * @param vaddr The virtual address.
 * @return pde_t* Pointer to the PDT entry. Returns nullptr if the entry is not present or if a large page is mapped.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE pde_t* get_pdt_entry(void* vaddr);

/**
 * @brief Retrieves the PTE (Page Table Entry) for a given virtual address.
 * 
 * @param vaddr The virtual address.
 * @return pte_t* Pointer to the PTE. Returns nullptr if the entry is not present or if a large page is mapped.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE pte_t* get_pte_entry(void* vaddr);

/**
 * @brief Translates a virtual address to its corresponding physical address.
 *
 * This function traverses the paging hierarchy to locate the physical address that corresponds
 * to the provided virtual address. It handles both standard 4KB pages and large pages (2MB and 1GB).
 *
 * @param vaddr The virtual address to translate.
 * @return uintptr_t The physical address. Returns 0 if the translation fails.
 *
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uintptr_t get_physical_address(void* vaddr);

/**
 * @brief Creates a new page table hierarchy for a userland process.
 *
 * Initializes a new set of page tables for use by a middle or an
 * upper class userland process. This means that the unprivileged
 * parts of the kernel are also mapped.
 *
 * @return page_table* Physical address to the newly created PML4 table. 
 *                     Returns nullptr if the allocation fails.
 *
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE page_table* create_higher_class_userland_page_table();

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
 * @param mbi_mmap_tag     On legacy systems where EFI is not available, a fallback legacy memory map is provided.
 * @param mbi_start_vaddr  Starting higher half address of the multiboot information structure passed by
 *                         GRUB to the kernel.
 * @param mbi_size         Size the multiboot information structure passed by GRUB to the kernel.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_physical_allocator(
    void* mbi_efi_mmap_tag,
    void* mbi_mmap_tag,
    uintptr_t mbi_start_vaddr,
    size_t mbi_size
);

/**
 * @brief Initializes the virtual memory allocator.
 * 
 * Prepares the virtual allocator to handle future requests for virtual address space.
 * Proper initialization is essential for enabling dynamic allocation and deallocation.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_virtual_allocator();
} // namespace paging

#endif // PAGING_H

