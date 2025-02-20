#include <memory/paging.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/tlb.h>
#include <memory/page_bitmap.h>
#include <memory/allocators/page_bootstrap_allocator.h>
#include <memory/allocators/heap_allocator.h>
#include <memory/allocators/dma_allocator.h>
#include <boot/efi_memory_map.h>
#include <boot/legacy_memory_map.h>
#include <serial/serial.h>
#include <dynpriv/dynpriv.h>

/*
 * Address where the kernel is loaded (-2GB)
 */
#define KERNEL_LOAD_OFFSET 0xffffffff80000000

/*
 * Addresses from 0xffffffdf80000000 to 0xffffffff80000000 will be reserved
 * to provide linear mapping between physical and virtual addresses.
 */
#define KERNEL_LINEAR_MAPPING_OFFSET 0xffffffdf80000000

extern char __ksymstart;
extern char __ksymend;
extern char __privileged_kernel_region_start;
extern char __privileged_kernel_region_end;

namespace paging {
bool g_linear_address_translations_available = false;

void* phys_to_virt_linear(uintptr_t paddr) {
    if (g_linear_address_translations_available) {
        return (void*)(paddr + KERNEL_LINEAR_MAPPING_OFFSET);
    }
    return (void*)paddr;
}

void* phys_to_virt_linear(void* paddr) {
    return phys_to_virt_linear(reinterpret_cast<uintptr_t>(paddr));
}

uintptr_t virt_to_phys_linear(void* vaddr) {
    if (g_linear_address_translations_available) {
        return (uintptr_t)vaddr - KERNEL_LINEAR_MAPPING_OFFSET;
    }
    return (uintptr_t)vaddr;
}

uintptr_t virt_to_phys_linear(uintptr_t vaddr) {
    return virt_to_phys_linear(reinterpret_cast<void*>(vaddr));
}


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
 * @brief Maps the first 1GB of physical memory using 2MB large pages.
 * 
 * This function establishes an identity mapping for the initial 1GB of physical memory
 * utilizing 2MB large pages. By doing so, it avoids the need to allocate additional physical
 * memory and prevents kernel bloat that would otherwise result from the numerous page table
 * entries required for mapping smaller memory regions. Empirical testing and observations
 * with GRUB2 showed that the first 1GB of RAM typically contains a sufficiently large
 * contiguous free region. This region accommodates the page frame bitmap and provides the
 * necessary space for a new page table, which is used to identity map the entire RAM and
 * the higher half of the kernel.
 */
__PRIVILEGED_CODE void bootstrap_map_first_1gb() {
    page_table* pml4 = get_pml4();

    pte_t* pml4_entry = &pml4->entries[0];
    page_table* pdpt = reinterpret_cast<page_table*>(PFN_TO_ADDR(pml4_entry->page_frame_number));

    pde_t* pdpt_entry = (pde_t*)&pdpt->entries[0];
    page_table* pdt = reinterpret_cast<page_table*>(PFN_TO_ADDR(pdpt_entry->page_frame_number));

    // Set up the 2MB large pages in the PD
    for (int i = 0; i < 512; ++i) { // 512 entries map 1GB with 2MB pages
        pde_t* pdt_entry = (pde_t*)&pdt->entries[i];
        pdt_entry->present = 1;
        pdt_entry->read_write = 1;
        pdt_entry->page_size = 1; // Large page (2MB)
        pdt_entry->page_frame_number = i * (0x200000 >> 12); // 2MB alignment
    }
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
    allocators::page_frame_allocator& allocator
) {
    virt_addr_indices_t indices = get_vaddr_page_table_indices(static_cast<uintptr_t>(vaddr));

    // Helper lambda to allocate a page table if not present
    auto get_page_table = [&allocator, &indices](pte_t& entry) -> page_table* {
        if (!(entry.value & PTE_PRESENT)) {  // Check if entry is not present
            auto* new_table = static_cast<page_table*>(
                phys_to_virt_linear(allocator.alloc_page())
            );

            if (!new_table) {
                serial::printf("[!] Failed to allocate physical frame for a page table!\n");
                return nullptr;
            }

            // Ensure that there is no leftover garbage data in the page
            zeromem(new_table, PAGE_SIZE);

            // Default flags for new page tables
            entry.value = PTE_PRESENT | PTE_RW | PTE_US;

            entry.page_frame_number = ADDR_TO_PFN(virt_to_phys_linear(new_table));
            return new_table;
        }

        return reinterpret_cast<page_table*>(
            phys_to_virt_linear(PFN_TO_ADDR(entry.page_frame_number))
        );
    };

    // Traverse and allocate as necessary
    page_table* pml4_table = reinterpret_cast<page_table*>(phys_to_virt_linear(pml4));
    page_table* pdpt = get_page_table(pml4_table->entries[indices.pml4]);
    page_table* pdt = get_page_table(pdpt->entries[indices.pdpt]);
    page_table* pt = get_page_table(pdt->entries[indices.pdt]);

    // Map the page with provided flags
    pte_t& pte = pt->entries[indices.pt];
    pte.value = flags; // First apply the flags
    pte.page_frame_number = ADDR_TO_PFN(paddr);

    // Invalidate the TLB entry for the virtual address
    if (get_pml4() == pml4) {
        invlpg(reinterpret_cast<void*>(vaddr));
    }
}

__PRIVILEGED_CODE
void map_pages(
    uintptr_t vaddr,
    uintptr_t paddr,
    size_t num_pages,
    uint64_t flags,
    page_table* pml4,
    allocators::page_frame_allocator& allocator
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

__PRIVILEGED_CODE
void map_large_page(
    uintptr_t vaddr,
    uintptr_t paddr,
    uint64_t flags,
    page_table* pml4,
    allocators::page_frame_allocator& allocator
) {
    // Get the indices for the given virtual address
    virt_addr_indices_t indices = get_vaddr_page_table_indices(static_cast<uintptr_t>(vaddr));

    // Helper lambda to allocate a page table if not present
    auto get_page_table = [&allocator, &indices](pte_t& entry) -> page_table* {
        if (!(entry.value & PTE_PRESENT)) {  // Check if entry is not present
            auto* new_table = static_cast<page_table*>(
                phys_to_virt_linear(allocator.alloc_page())
            );

            if (!new_table) {
                serial::printf("[!] Failed to allocate physical frame for a page table!\n");
                return nullptr;
            }

            // Ensure that there is no leftover garbage data in the page
            zeromem(new_table, PAGE_SIZE);

            // Default flags for new page tables
            entry.value = PTE_PRESENT | PTE_RW | PTE_US;

            entry.page_frame_number = ADDR_TO_PFN(virt_to_phys_linear(new_table));
            return new_table;
        }

        return reinterpret_cast<page_table*>(
            phys_to_virt_linear(PFN_TO_ADDR(entry.page_frame_number))
        );
    };

    // Traverse and allocate PML4 and PDPT as necessary
    page_table* pml4_table = reinterpret_cast<page_table*>(phys_to_virt_linear(pml4));
    page_table* pdpt = get_page_table(pml4_table->entries[indices.pml4]);
    page_table* pdt = get_page_table(pdpt->entries[indices.pdpt]);

    // Map the large page with provided flags directly in the PDT
    pte_t& pde = pdt->entries[indices.pdt];
    pde.value = flags | PTE_PS; // Apply flags and set the page size (PS) bit
    pde.page_frame_number = ADDR_TO_PFN(paddr); // Set the physical frame number

    // Invalidate the TLB entry for the virtual address
    if (get_pml4() == pml4) {
        invlpg(reinterpret_cast<void*>(vaddr));
    }
}

__PRIVILEGED_CODE
pde_t* get_pml4_entry(void* vaddr) {
    page_table* pml4 = reinterpret_cast<page_table*>(phys_to_virt_linear(get_pml4()));
    virt_addr_indices_t indices = get_vaddr_page_table_indices(reinterpret_cast<uint64_t>(vaddr));
    return reinterpret_cast<pde_t*>(&pml4->entries[indices.pml4]);
}

__PRIVILEGED_CODE
pde_t* get_pdpt_entry(void* vaddr) {
    pde_t* pml4_entry = get_pml4_entry(vaddr);
    
    if (!pml4_entry || !(pml4_entry->present)) {
        return nullptr;
    }

    page_table* pdpt = reinterpret_cast<page_table*>(
        phys_to_virt_linear(PFN_TO_ADDR(pml4_entry->page_frame_number))
    );
    virt_addr_indices_t indices = get_vaddr_page_table_indices(reinterpret_cast<uint64_t>(vaddr));
    
    return reinterpret_cast<pde_t*>(&pdpt->entries[indices.pdpt]);
}

__PRIVILEGED_CODE
pde_t* get_pdt_entry(void* vaddr) {
    pde_t* pdpt_entry = get_pdpt_entry(vaddr);
    if (!pdpt_entry || !(pdpt_entry->present)) {
        return nullptr;
    }
    
    // Check if this PDPT entry maps a 1GB large page
    if (pdpt_entry->page_size) {
        // Large page (1GB) is mapped; PDT is not used
        return nullptr;
    }
    
    page_table* pdt = reinterpret_cast<page_table*>(
        phys_to_virt_linear(PFN_TO_ADDR(pdpt_entry->page_frame_number))
    );
    virt_addr_indices_t indices = get_vaddr_page_table_indices(reinterpret_cast<uint64_t>(vaddr));

    return reinterpret_cast<pde_t*>(&pdt->entries[indices.pdt]);
}

__PRIVILEGED_CODE pte_t* get_pte_entry(void* vaddr) {
    pde_t* pdt_entry = get_pdt_entry(vaddr);
    if (!pdt_entry || !(pdt_entry->present)) {
        return nullptr;
    }

    // Check if this PDT entry maps a 2MB large page
    if (pdt_entry->page_size) {
        // Large page (2MB) is mapped; PT is not used
        return nullptr;
    }

    page_table* pt = reinterpret_cast<page_table*>(
        phys_to_virt_linear(PFN_TO_ADDR(pdt_entry->page_frame_number))
    );
    virt_addr_indices_t indices = get_vaddr_page_table_indices(reinterpret_cast<uint64_t>(vaddr));

    return &pt->entries[indices.pt];
}

__PRIVILEGED_CODE
uintptr_t get_physical_address(void* vaddr) {
    uint64_t virtual_addr = reinterpret_cast<uint64_t>(vaddr);

    //
    // If the address is greater than KERNEL_LOAD_OFFSET, then a simple linear
    // conversion can be performed since it's mapped 1-to-1 to physical memory.
    //
    if (virtual_addr >= KERNEL_LOAD_OFFSET) {
        return virtual_addr - KERNEL_LOAD_OFFSET;
    }

    virt_addr_indices_t indices = get_vaddr_page_table_indices(virtual_addr);

    // Retrieve PML4 entry
    pde_t* pml4_entry = get_pml4_entry(vaddr);
    if (!pml4_entry || !(pml4_entry->present)) {
        return 0; // Invalid mapping
    }

    // Retrieve PDPT entry
    pde_t* pdpt_entry = get_pdpt_entry(vaddr);
    if (!pdpt_entry || !(pdpt_entry->present)) {
        return 0; // Invalid mapping
    }

    // Check for 1GB large page
    if (pdpt_entry->page_size) {
        uintptr_t phys_base = PFN_TO_ADDR(pdpt_entry->page_frame_number);
        uintptr_t offset = virtual_addr & 0x3FFFFFFF; // Offset within 1GB
        return phys_base + offset;
    }

    // Retrieve PDT entry
    pde_t* pdt_entry = get_pdt_entry(vaddr);
    if (!pdt_entry || !(pdt_entry->present)) {
        return 0; // Invalid mapping
    }

    // Check for 2MB large page
    if (pdt_entry->page_size) {
        uintptr_t phys_base = PFN_TO_ADDR(pdt_entry->page_frame_number);
        uintptr_t offset = virtual_addr & 0x1FFFFF; // Offset within 2MB
        return phys_base + offset;
    }

    // Retrieve PTE entry
    pte_t* pte_entry = get_pte_entry(vaddr);
    if (!pte_entry || !(pte_entry->present)) {
        return 0; // Invalid mapping
    }

    // Calculate the physical address
    uintptr_t phys_base = PFN_TO_ADDR(pte_entry->page_frame_number);
    uintptr_t offset = virtual_addr & 0xFFF; // Offset within 4KB
    return phys_base + offset;
}

__PRIVILEGED_CODE
page_table* create_higher_class_userland_page_table() {
    // Get the current page table
    page_table* current_pt = reinterpret_cast<page_table*>(
        phys_to_virt_linear(paging::get_pml4())
    );

    // Allocate a page for the new top-level page table
    page_table* new_pt = reinterpret_cast<page_table*>(
        vmm::alloc_linear_mapped_persistent_page()
    );

    // Zero out and prepare a clean page
    zeromem(new_pt, sizeof(page_table));

    // Copy the page directory pointer table responsible for
    // the kernel's higher-half address region.
    new_pt->entries[511] = current_pt->entries[511];

    return reinterpret_cast<page_table*>(virt_to_phys_linear(new_pt));
}

__PRIVILEGED_CODE
void init_physical_allocator(
    void* mbi_efi_mmap_tag,
    void* mbi_mmap_tag,
    uintptr_t mbi_start_vaddr,
    size_t mbi_size
) {
    // Identity map the first 1GB of physical RAM memory for further bootstrapping
    bootstrap_map_first_1gb();

    // Flush the entire TLB
    tlb_flush_all();
    
    // Create
    efi_memory_map efi_memmap(reinterpret_cast<multiboot_tag_efi_mmap*>(mbi_efi_mmap_tag));
    legacy_memory_map legacy_memmap(reinterpret_cast<multiboot_tag_mmap*>(mbi_mmap_tag));

    // Then select which one we want via a pointer
    boot_memory_map* memory_map = nullptr;

    if (mbi_efi_mmap_tag) {
        memory_map = &efi_memmap;
    } else {
        memory_map = &legacy_memmap;
    }

    // Debug print the EFI memory map
    memory_map->print_memory_map();

    // Kernel memory region details
    uint64_t kernel_start = reinterpret_cast<uint64_t>(&__ksymstart);
    uint64_t kernel_end = reinterpret_cast<uint64_t>(&__ksymend);
    uint64_t kernel_size = kernel_end - kernel_start;

    // Calculate the size needed for the page frame bitmap
    uint64_t page_bitmap_size = page_frame_bitmap::calculate_required_size(
        memory_map->get_highest_address()
    );

    // Calculate memory required for page tables (all of RAM + kernel higher-half mappings)
    uint64_t page_table_size = compute_page_table_memory(
        memory_map->get_total_system_memory() + kernel_size + mbi_size
    );

    // Access largest conventional memory segment
    // The segment has to be large enough to fit the bitmap and the full page table covering system memory
    memory_map_descriptor largest_segment = memory_map->find_segment_for_allocation_block(
        10 * (1 << 20),     // Start searching from the 10MB point to be guaranteed above the kernel
        1ULL << 30,         // Pick the largest segment within the 1GB range
        page_bitmap_size + page_table_size
    );

    // Ensure that the segment actually exists
    if (!largest_segment.mem_available) {
        serial::printf("[*] No conventional memory segments found!\n");
        return;
    }

#if 0
    serial::printf(
        "Largest Conventional Memory Segment:\n"
        "  Size: %llu MB (%llu pages)\n"
        "  Physical: 0x%016llx - 0x%016llx\n",
        largest_segment.length / (1024 * 1024), largest_segment.length / PAGE_SIZE,
        largest_segment.paddr, largest_segment.paddr + largest_segment.length
    );
#endif

    // Initialize the bootstrap allocator to the region of memory right after the page bitmap
    auto& bootstrap_allocator = allocators::page_bootstrap_allocator::get();
    bootstrap_allocator.init(largest_segment.base_addr + page_bitmap_size, page_table_size);

    // Allocate a new top-level page table
    page_table* new_pml4 = reinterpret_cast<page_table*>(bootstrap_allocator.alloc_page());

    // Identity map the physical addresses in RAM
    for (size_t i = 0; i < memory_map->get_num_entries(); i++) {
        const auto entry = memory_map->get_entry_desc(i);

        if (entry.mem_available) {
            for (uint64_t vaddr = entry.base_addr; vaddr < entry.base_addr + entry.length; vaddr += PAGE_SIZE) {
                map_page(vaddr, vaddr, PTE_DEFAULT_PRIV_KERNEL_FLAGS, new_pml4, bootstrap_allocator);
            }
        }
    }

    // Keep track of privileged kernel regions
    uintptr_t priv_region_start = reinterpret_cast<uintptr_t>(&__privileged_kernel_region_start);
    uintptr_t priv_region_end = reinterpret_cast<uintptr_t>(&__privileged_kernel_region_end);

    // Create the higher-half mappings for the kernel
    for (uint64_t vaddr = KERNEL_LOAD_OFFSET; vaddr < reinterpret_cast<uint64_t>(&__ksymend); vaddr += PAGE_SIZE) {
        uintptr_t paddr = vaddr - KERNEL_LOAD_OFFSET;
        uint64_t flags = PTE_DEFAULT_UNPRIV_KERNEL_FLAGS;

        if (vaddr >= priv_region_start && vaddr < priv_region_end) {
            flags = PTE_DEFAULT_PRIV_KERNEL_FLAGS;
        }

        map_page(vaddr, paddr, flags, new_pml4, bootstrap_allocator);
    }

    // Create the higher half mappings for the MBI data structure
    size_t mbi_size_page_aligned = PAGE_ALIGN(mbi_size);
    uintptr_t mbi_start_paddr = mbi_start_vaddr - KERNEL_LOAD_OFFSET;
    uintptr_t mbi_end_paddr = mbi_start_paddr + mbi_size_page_aligned;

    for (uint64_t mbi_paddr = mbi_start_paddr; mbi_paddr < mbi_end_paddr; mbi_paddr += PAGE_SIZE) {
        uint64_t mbi_high_half_addr = mbi_paddr + KERNEL_LOAD_OFFSET;
        map_page(mbi_high_half_addr, mbi_paddr, PTE_DEFAULT_PRIV_KERNEL_FLAGS, new_pml4, bootstrap_allocator);
    }

    // Install the new page table
    set_pml4(new_pml4);

    // Set the new blessed kernel ASID in the dynamic privilege subsystem
    dynpriv::set_blessed_kernel_asid();

    // Initialize the page frame bitmap
    auto& bitmap_allocator = allocators::page_bitmap_allocator::get_physical_allocator();
    bitmap_allocator.init_bitmap(page_bitmap_size, reinterpret_cast<uint8_t*>(largest_segment.base_addr), true);
    bitmap_allocator.mark_bitmap_address_as_physical();

    // Since the bitmap starts with all memory marked as 'used', we
    // have to unlock all memory regions marked as Available.
    for (size_t i = 0; i < memory_map->get_num_entries(); i++) {
        const auto entry = memory_map->get_entry_desc(i);

        // Unlock only those pages that are part of EfiConventionalMemory
        if (entry.mem_available) {
            bitmap_allocator.free_pages(
                reinterpret_cast<void*>(entry.base_addr),
                entry.length / PAGE_SIZE
            );
        }
    }

    // Lock pages belonging to the kernel
    uintptr_t kernel_physical_start = reinterpret_cast<uintptr_t>(&__ksymstart) - KERNEL_LOAD_OFFSET;
    bitmap_allocator.lock_pages(
        reinterpret_cast<void*>(kernel_physical_start),
        (kernel_size / PAGE_SIZE) + 1
    );

    // Lock pages belonging to the page frame bitmap
    uintptr_t bitmap_physical_start = largest_segment.base_addr;
    bitmap_allocator.lock_pages(
        reinterpret_cast<void*>(bitmap_physical_start),
        (page_bitmap_size / PAGE_SIZE) + 1
    );

    // Lock pages belonging to the new page table
    uintptr_t page_table_physical_start = largest_segment.base_addr + page_bitmap_size;
    bitmap_allocator.lock_pages(
        reinterpret_cast<void*>(page_table_physical_start),
        (page_table_size / PAGE_SIZE) + 1
    );
}

__PRIVILEGED_CODE
void init_virtual_allocator() {
    /*
    * First we have to prepare the memory for the virtual allocator's bitmap.
    *
    * Available Kernel VAS (virtual address space) starts at 0xffffff8000000000 and ends
    * at 0xffffffff80000000, providing around 510 GB of addressable virtual memory.
    * In order to support that, the bitmap requires 133,693,440 bits (one bit per 4 KB page).
    * This means that the bitmap would require 16 MB of backing memory to store it, so
    * an efficient way to do this would be to map 8 large pages (2 MB each) for backing storage.
    * 
    * Additionally, addresses from 0xffffffdf80000000 to 0xffffffff80000000 will be reserved
    * to provide linear mapping between physical and virtual addresses.
    * This mapping is valid for up to 128GB of physical addresses.
    * 
    * The full kernel VAS layout is represented below:
    *  +----------------------------+  0xFFFFFFFFFFFFFFFF
    *  |      Kernel Image Region   |
    *  |                            |
    *  |    [Kernel code, data]     |
    *  +----------------------------+  0xFFFFFFFF80000000
    *  |    Linear Mapping Region   |
    *  |                            |
    *  | [128GB of phys. addresses] |
    *  +----------------------------+  0xFFFFFFDF80000000
    *  |  VMM and Virtual Allocator |
    *  |                            |
    *  | [Heap, temp maps, others]  |
    *  +----------------------------+  0xFFFFFF8000000000
    */

    const uint64_t large_page_size = 2 * 1024 * 1024; // 2MB
    const uint64_t num_large_pages = 8;               // 16MB / 2MB = 8

    for (uint64_t i = 0; i < num_large_pages; ++i) {
        // Calculate the virtual address for this page
        uintptr_t vaddr = KERN_VIRT_BASE + (i * large_page_size);

        // Allocate a 2MB large page
        auto& physical_allocator = allocators::page_bitmap_allocator::get_physical_allocator();
        void* paddr = physical_allocator.alloc_large_page();
        if (paddr == nullptr) {
            serial::printf("[!] Failed to allocate large page for virtual address: 0x%016llx\n", vaddr);
            continue;
        }

        // Map the allocated physical page to the virtual address
        map_large_page(vaddr, reinterpret_cast<uintptr_t>(paddr), PTE_DEFAULT_PRIV_KERNEL_FLAGS, get_pml4());
    }

    auto& virtual_allocator = allocators::page_bitmap_allocator::get_virtual_allocator();
    virtual_allocator.init_bitmap(large_page_size * num_large_pages, reinterpret_cast<uint8_t*>(KERN_VIRT_BASE));

    // Make sure the pages that this allocator tracks starts at KERN_VIRT_BASE (0xffffff8000000000)
    virtual_allocator.set_base_page_offset(KERN_VIRT_BASE);

    // Lock the virtual address space region that references the allocator's bitmap
    const size_t bitmap_page_count = (large_page_size * num_large_pages) / PAGE_SIZE; // 16MB / 4KB = 4096
    virtual_allocator.lock_pages(reinterpret_cast<void*>(KERN_VIRT_BASE), bitmap_page_count);

    // Initialize the main kernel heap
    auto& kernel_heap = allocators::heap_allocator::get();
    kernel_heap.init(0x0, KERNEL_HEAP_INIT_SIZE);

    // Create the DMA pools
    auto& dma = allocators::dma_allocator::get();
    dma.init();

    // Lock the kernel VAS region reserved for linear mapping physical addresses
    virtual_allocator.lock_pages(
        reinterpret_cast<void*>(KERNEL_LINEAR_MAPPING_OFFSET),
        (KERNEL_LOAD_OFFSET - KERNEL_LINEAR_MAPPING_OFFSET) / PAGE_SIZE
    );

    // Map using 2MB pages the kernel VAS region reserved for linear mapping physical addresses
    for (size_t i = KERNEL_LINEAR_MAPPING_OFFSET; i < KERNEL_LOAD_OFFSET; i += LARGE_PAGE_SIZE) {
        uintptr_t vaddr = i;
        uintptr_t paddr = i - KERNEL_LINEAR_MAPPING_OFFSET;

        // Ensure the mapped virtual pages are privileged
        paging::map_large_page(vaddr, paddr, PTE_DEFAULT_PRIV_KERNEL_FLAGS, get_pml4());
    }

    // Indicate that the linear address translations
    // have been initialized and are now available.
    g_linear_address_translations_available = true;

    // Initialize the dynamic privilege ASID whitelist data
    // structures that require dynamic memory allocator.
    dynpriv::initialize_dynpriv_asid_whitelist();
}
} // namespace paging

