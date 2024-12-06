#ifndef PAGING_H
#define PAGING_H
#include "allocators/page_bitmap_allocator.h"

#define PAGE_SIZE   0x1000
#define PAGE_OFFSET 0xffffff8000000000

#define PAGE_ALIGN(value) (((value) + (PAGE_SIZE) - 1) & ~((PAGE_SIZE) - 1))

#define PAGE_TABLE_ENTRIES 512

#define USER_PAGE    1
#define KERNEL_PAGE  0

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
 * @brief Retrieves the current PML4 table by reading the CR3 register.
 * 
 * This function reads the CR3 register, which contains the physical address of the current
 * PML4 (Page Map Level 4) table, and casts it to a pointer to `page_table`.
 * 
 * @return A physical address pointer to the current PML4 `page_table`.
 */
__PRIVILEGED_CODE page_table* get_pml4();

/**
 * @brief Sets the current PML4 table by writing to the CR3 register.
 * 
 * This function takes a pointer to a `page_table` (PML4) and writes its
 * address to the CR3 register.
 * 
 * @param pml4 A physical address pointer to the `page_table` to be set as the new PML4.
 */
__PRIVILEGED_CODE void set_pml4(page_table* pml4);

__PRIVILEGED_CODE
void map_page(
    uintptr_t vaddr,
    uintptr_t paddr,
    page_table* pml4,
    allocators::phys_frame_allocator& allocator = allocators::page_bitmap_allocator::get()
);

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

