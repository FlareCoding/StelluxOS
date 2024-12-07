#include <memory/paging.h>
#include <memory/memory.h>
#include <memory/tlb.h>
#include <memory/page_bitmap.h>
#include <memory/allocators/page_bootstrap_allocator.h>
#include <boot/efimem.h>
#include <serial/serial.h>

#define KERNEL_LOAD_OFFSET 0xffffffff80000000

extern char __ksymstart;
extern char __ksymend;

namespace paging {
virt_addr_indices_t get_vaddr_page_table_indices(uint64_t vaddr) {
    virt_addr_indices_t indices;
    indices.pml4 = (vaddr >> 39) & 0x1FF;
    indices.pdpt = (vaddr >> 30) & 0x1FF;
    indices.pdt = (vaddr >> 21) & 0x1FF;
    indices.pt = (vaddr >> 12) & 0x1FF;
    return indices;
}

__PRIVILEGED_CODE
page_table* get_pml4() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return reinterpret_cast<page_table*>(cr3);
}

__PRIVILEGED_CODE
void set_pml4(page_table* pml4) {
    uint64_t cr3 = reinterpret_cast<uint64_t>(pml4);
    asm volatile("mov %0, %%cr3" :: "r"(cr3));
}

/**
 * @brief Maps the first 1GB of physical memory using 1GB huge pages.
 * 
 * This function establishes an identity mapping for the initial 1GB of physical memory
 * utilizing 1GB huge pages. By doing so, it avoids the need to allocate additional physical
 * memory and prevents kernel bloat that would otherwise result from the numerous page table
 * entries required for mapping smaller memory regions. Empirical testing and observations
 * with GRUB2 indicate that the first 1GB of RAM typically contains a sufficiently large
 * contiguous free region. This region accommodates the page frame bitmap and provides the
 * necessary space for a new page table, which is used to identity map the entire RAM and
 * the higher half of the kernel.
 */
void bootstrap_map_first_1gb() {
    page_table* pml4 = get_pml4();

    pte_t* pml4_entry = &pml4->entries[0];
    page_table* pdpt = (page_table*)(((uint64_t)pml4_entry->page_frame_number << 12));

    pde_t* pdpt_entry = (pde_t*)&pdpt->entries[0];
    pdpt_entry->present = 1;
    pdpt_entry->read_write = 1;
    pdpt_entry->page_size = 1; // Large page (1GB)
    pdpt_entry->page_frame_number = 0;
}

/**
 * @brief Calculates the total memory required for page tables to map a given memory size.
 *
 * @param memory_to_map The amount of memory (in bytes) that the page tables need to handle.
 * @return The total memory (in bytes) required for all page tables.
 */
uint64_t compute_page_table_memory(uint64_t memory_to_map) {
    const uint64_t entry_size = 8; // Bytes per page table entry
    const uint64_t table_size = PAGE_TABLE_ENTRIES * entry_size;

    // Calculate the number of pages needed, rounding up
    uint64_t total_pages = (memory_to_map + PAGE_SIZE - 1) / PAGE_SIZE;

    // Initialize total tables with 1 for the PML4 table
    uint64_t total_tables = 1;
    uint64_t entries = total_pages;

    // Iterate through each level of the page table hierarchy (PDPT, PD, PT)
    for (int level = 0; level < 3; ++level) {
        entries = (entries + PAGE_TABLE_ENTRIES - 1) / PAGE_TABLE_ENTRIES;
        total_tables += entries;
    }

    // Calculate the total memory by multiplying the number of tables by the size of each table
    return PAGE_ALIGN(total_tables * table_size);
}

__PRIVILEGED_CODE
void map_page(
    uintptr_t vaddr,
    uintptr_t paddr,
    uint64_t flags,
    page_table* pml4,
    allocators::phys_frame_allocator& allocator
) {
    virt_addr_indices_t indices = get_vaddr_page_table_indices(static_cast<uintptr_t>(vaddr));

    // Helper lambda to allocate a page table if not present
    auto get_page_table = [&allocator](pte_t& entry) -> page_table* {
        if (!(entry.value & PTE_PRESENT)) {  // Check if entry is not present
            auto* new_table = static_cast<page_table*>(allocator.alloc_physical_page());
            entry.value = PTE_PRESENT | PTE_RW; // Default for new page tables
            entry.page_frame_number = ADDR_TO_PFN(reinterpret_cast<uintptr_t>(new_table));
            return new_table;
        }
        return reinterpret_cast<page_table*>(PFN_TO_ADDR(entry.page_frame_number));
    };

    // Traverse and allocate as necessary
    page_table* pdpt = get_page_table(pml4->entries[indices.pml4]);
    page_table* pdt = get_page_table(pdpt->entries[indices.pdpt]);
    page_table* pt = get_page_table(pdt->entries[indices.pdt]);

    // Map the page with provided flags
    pte_t& pte = pt->entries[indices.pt];
    pte.value = flags; // First apply the flags
    pte.page_frame_number = ADDR_TO_PFN(paddr);

    // Invalidate the TLB entry for the virtual address
    invlpg(reinterpret_cast<void*>(vaddr));
}

__PRIVILEGED_CODE void map_pages(
    uintptr_t vaddr,
    uintptr_t paddr,
    size_t num_pages,
    uint64_t flags,
    page_table* pml4,
    allocators::phys_frame_allocator& allocator
) {
    for (size_t i = 0; i < num_pages; ++i) {
        map_page(
            vaddr + i * PAGE_SIZE,
            paddr + i * PAGE_SIZE,
            flags,
            pml4,
            allocator
        );
    }
}

__PRIVILEGED_CODE void init_physical_allocator(void* mbi_efi_mmap_tag) {
    // Identity map the first 1GB of physical RAM memory using a huge page
    bootstrap_map_first_1gb();

    // Flush the entire TLB
    tlb_flush_all();

    auto efi_mmap_tag = reinterpret_cast<multiboot_tag_efi_mmap*>(mbi_efi_mmap_tag);
    efi::efi_memory_map memory_map(efi_mmap_tag);

    // Debug print the EFI memory map
    memory_map.print_memory_map();

    // Kernel memory region details
    uint64_t kernel_start = reinterpret_cast<uint64_t>(&__ksymstart);
    uint64_t kernel_end = reinterpret_cast<uint64_t>(&__ksymend);
    uint64_t kernel_size = kernel_end - kernel_start;

    // Calculate the size needed for the page frame bitmap
    uint64_t page_bitmap_size = page_frame_bitmap::calculate_required_size(
        memory_map.get_highest_address()
    );

    // Calculate memory required for page tables (all of RAM + kernel higher-half mappings)
    uint64_t page_table_size = compute_page_table_memory(
        memory_map.get_total_system_memory() + kernel_size
    );

    // Access largest conventional memory segment
    // The segment has to be large enough to fit the bitmap and the full page table covering system memory
    efi::efi_memory_descriptor_wrapper largest_segment = memory_map.find_segment_for_allocation_block(
        10 * (1 << 20),     // Start searching from the 10MB point to be guaranteed above the kernel
        1ULL << 30,         // Pick the largest segment within the 1GB range
        page_bitmap_size + page_table_size
    );

    // Ensure that the segment actually exists
    if (!largest_segment.desc) {
        serial::com1_printf("[*] No conventional memory segments found!\n");
        return;
    }

#if 0
    serial::com1_printf(
        "Largest Conventional Memory Segment:\n"
        "  Size: %llu MB (%llu pages)\n"
        "  Physical: 0x%016llx - 0x%016llx\n",
        largest_segment.length / (1024 * 1024), largest_segment.length / PAGE_SIZE,
        largest_segment.paddr, largest_segment.paddr + largest_segment.length
    );
#endif

    // Initialize the bootstrap allocator to the region of memory right after the page bitmap
    auto& bootstrap_allocator = allocators::page_bootstrap_allocator::get();
    bootstrap_allocator.init(largest_segment.paddr + page_bitmap_size, page_table_size);

    // Allocate a new top-level page table
    page_table* new_pml4 = reinterpret_cast<page_table*>(bootstrap_allocator.alloc_physical_page());

    // Identity map the physical addresses in RAM
    for (const auto& entry : memory_map) {
        if (entry.desc->type == EFI_MEMORY_TYPE_CONVENTIONAL_MEMORY) {
            for (uint64_t vaddr = entry.paddr; vaddr < entry.paddr + entry.length; vaddr += PAGE_SIZE) {
                map_page(vaddr, vaddr, PTE_DEFAULT_KERNEL_FLAGS, new_pml4, bootstrap_allocator);
            }
        }
    }

    // Create the higher-half mappings for the kernel
    for (uint64_t vaddr = KERNEL_LOAD_OFFSET; vaddr < reinterpret_cast<uint64_t>(&__ksymend); vaddr += PAGE_SIZE) {
        uintptr_t paddr = vaddr - KERNEL_LOAD_OFFSET;
        map_page(vaddr, paddr, PTE_DEFAULT_KERNEL_FLAGS, new_pml4, bootstrap_allocator);
    }

    // Install the new page table
    set_pml4(new_pml4);

    // Initialize the page frame bitmap
    page_frame_bitmap::get().init(page_bitmap_size, reinterpret_cast<uint8_t*>(largest_segment.paddr));

    auto& bitmap_allocator = allocators::page_bitmap_allocator::get();

    // Since the bitmap starts with all memory marked as 'used', we
    // have to unlock all memory regions marked as EfiConventionalMemory.
    for (const auto& entry : memory_map) {
        if (entry.desc->type == EFI_MEMORY_TYPE_CONVENTIONAL_MEMORY) {
            // Lock any page that is not part of EfiConventionalMemory
            bitmap_allocator.free_physical_pages(
                reinterpret_cast<void*>(entry.paddr),
                entry.desc->page_count
            );
        }
    }

    // Lock pages belonging to the kernel
    uintptr_t kernel_physical_start = reinterpret_cast<uintptr_t>(&__ksymstart) - KERNEL_LOAD_OFFSET;
    bitmap_allocator.lock_physical_pages(
        reinterpret_cast<void*>(kernel_physical_start),
        (kernel_size / PAGE_SIZE) + 1
    );

    // Lock pages belonging to the page frame bitmap
    uintptr_t bitmap_physical_start = largest_segment.paddr;
    bitmap_allocator.lock_physical_pages(
        reinterpret_cast<void*>(bitmap_physical_start),
        (page_bitmap_size / PAGE_SIZE) + 1
    );

    // Lock pages belonging to the new page table
    uintptr_t page_table_physical_start = largest_segment.paddr + page_bitmap_size;
    bitmap_allocator.lock_physical_pages(
        reinterpret_cast<void*>(page_table_physical_start),
        (page_table_size / PAGE_SIZE) + 1
    );
}
} // namespace paging

