#include <memory/paging.h>
#include <memory/memory.h>
#include <serial/serial.h>
#include <memory/page_bitmap.h>
#include <memory/allocators/page_bootstrap_allocator.h>
#include <boot/efimem.h>

extern char __ksymstart;
extern char __ksymend;

namespace paging {
virt_addr_indices_t get_vaddr_page_table_indices(uint64_t virt_addr) {
    virt_addr_indices_t indices;
    indices.pml4 = (virt_addr >> 39) & 0x1FF;
    indices.pdpt = (virt_addr >> 30) & 0x1FF;
    indices.pdt = (virt_addr >> 21) & 0x1FF;
    indices.pt = (virt_addr >> 12) & 0x1FF;
    return indices;
}

void map_1gb_page(void* vaddr, void* paddr) {
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    page_table* pml4 = (page_table*)cr3;

    virt_addr_indices_t vaddr_indices =
        get_vaddr_page_table_indices(reinterpret_cast<uintptr_t>(vaddr));

    pte_t* pml4_entry = &pml4->entries[vaddr_indices.pml4];
    page_table* pdpt = (page_table*)(((uint64_t)pml4_entry->page_frame_number << 12));

    pde_t* pdpt_entry = (pde_t*)&pdpt->entries[vaddr_indices.pdpt];
    pdpt_entry->present = 1;
    pdpt_entry->read_write = 1;
    pdpt_entry->page_size = 1; // Large page (1GB)
    pdpt_entry->page_frame_number = reinterpret_cast<uint64_t>(paddr) >> 30;
}

uint64_t calculate_page_table_memory(uint64_t total_system_size) {
    const uint64_t page_size = 4096; // 4 KB pages
    const uint64_t entries_per_table = 512; // 512 entries per page table
    const uint64_t entry_size = 8; // 8 bytes per entry
    const uint64_t table_size = entries_per_table * entry_size; // Size of one page table (4 KB)

    // Calculate the number of pages required
    uint64_t total_pages = (total_system_size + page_size - 1) / page_size;

    // Calculate the number of page tables required
    uint64_t page_tables = (total_pages + entries_per_table - 1) / entries_per_table;

    // Calculate the number of page directories required
    uint64_t page_directories = (page_tables + entries_per_table - 1) / entries_per_table;

    // Calculate the number of PDPTs required
    uint64_t pdpts = (page_directories + entries_per_table - 1) / entries_per_table;

    // Only one PML4 table is required
    uint64_t pml4_tables = 1;

    // Total memory for all tables
    uint64_t total_memory = (page_tables + page_directories + pdpts + pml4_tables) * table_size;

    return total_memory;
}

__PRIVILEGED_CODE
void map_page(
    uintptr_t vaddr,
    uintptr_t paddr,
    page_table* pml4,
    allocators::phys_frame_allocator& allocator
) {
    virt_addr_indices_t vaddr_indices =
        get_vaddr_page_table_indices(reinterpret_cast<uintptr_t>(vaddr));

	page_table *pdpt = nullptr, *pdt = nullptr, *pt = nullptr;

	pte_t* pml4_entry = &pml4->entries[vaddr_indices.pml4];

	if (pml4_entry->present == 0) {
		pdpt = (page_table*)allocator.alloc_physical_page();

		pml4_entry->present = 1;
		pml4_entry->read_write = 1;
		pml4_entry->page_frame_number = reinterpret_cast<uint64_t>(pdpt) >> 12;
	} else {
		pdpt = (page_table*)((uint64_t)pml4_entry->page_frame_number << 12);
	}

	pte_t* pdpt_entry = &pdpt->entries[vaddr_indices.pdpt];
	
	if (pdpt_entry->present == 0) {
		pdt = (page_table*)allocator.alloc_physical_page();

		pdpt_entry->present = 1;
		pdpt_entry->read_write = 1;
		pdpt_entry->page_frame_number = reinterpret_cast<uint64_t>(pdt) >> 12;
	} else {
		pdt = (page_table*)((uint64_t)pdpt_entry->page_frame_number << 12);
	}

	pte_t* pdt_entry = &pdt->entries[vaddr_indices.pdt];
	
	if (pdt_entry->present == 0) {
		pt = (page_table*)allocator.alloc_physical_page();

		pdt_entry->present = 1;
		pdt_entry->read_write = 1;
		pdt_entry->page_frame_number = reinterpret_cast<uint64_t>(pt) >> 12;
	} else {
		pt = (page_table*)((uint64_t)pdt_entry->page_frame_number << 12);
	}

	pte_t* pte = &pt->entries[vaddr_indices.pt];
	pte->present = 1;
	pte->read_write = 1;
	pte->page_frame_number = reinterpret_cast<uint64_t>(paddr) >> 12;
}

__PRIVILEGED_CODE void init_physical_allocator(void* mbi_efi_mmap_tag) {
    // Identity map the first 1GB of physical RAM memory using a huge page
    map_1gb_page(0x0, 0x0);

    // Flush the entire TLB
    asm volatile ("mov %cr3, %rax");
    asm volatile ("mov %rax, %cr3");

    multiboot_tag_efi_mmap* efi_mmap_tag = reinterpret_cast<multiboot_tag_efi_mmap*>(mbi_efi_mmap_tag);
    efi::efi_memory_map memory_map(efi_mmap_tag);

    serial::com1_printf("  EFI Memory Map:\n");

    for (const auto& entry : memory_map) {
        // Filter for EfiConventionalMemory (type 7)
        if (entry.desc->type != 7) {
            continue;
        }

        uint64_t physical_start = entry.paddr;
        uint64_t length = entry.length;

        serial::com1_printf(
            "  Type: %u, Size: %llu MB (%llu pages)\n"
            "  Physical: 0x%016llx - 0x%016llx\n"
            "  Virtual:  0x%016llx - 0x%016llx\n",
            entry.desc->type, length / (1024 * 1024), length / 4096,
            physical_start, physical_start + length,
            physical_start + 0xffffff8000000000, physical_start + length + 0xffffff8000000000);
    }

    // Access total system conventional memory
    uint64_t total_system_size_mb = memory_map.get_total_system_memory() / (1024 * 1024);
    uint64_t total_system_conventional_size_mb = memory_map.get_total_conventional_memory() / (1024 * 1024);
    serial::com1_printf("\nTotal System Memory                : %llu MB\n", total_system_size_mb);
    serial::com1_printf("Total System Conventional Memory   : %llu MB\n", total_system_conventional_size_mb);

    uint64_t kernel_start = (uint64_t)&__ksymstart;
    uint64_t kernel_end = (uint64_t)&__ksymend;
    uint64_t kernel_page_count = (kernel_end - kernel_start) / PAGE_SIZE;
    serial::com1_printf("Kernel Size: %llu KB\n    ", (kernel_end - kernel_start) / 1024);
    serial::com1_printf(
        "0x%016llx-0x%016llx (%d pages)\n",
        kernel_start - 0xffffffff80000000, kernel_end - 0xffffffff80000000,
        kernel_page_count
    );

    // Calculate the size needed for the page frame bitmap
    uint64_t page_bitmap_size = memory_map.get_total_conventional_memory() / PAGE_SIZE / 8 + 1;
    // Round up to the nearest page size
    page_bitmap_size = (page_bitmap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    serial::com1_printf("\nPage Bitmap Size: %llu KB\n", page_bitmap_size / 1024);

    uint64_t page_table_memory = calculate_page_table_memory(memory_map.get_total_system_memory() + kernel_page_count * PAGE_SIZE);
    // Round up to the nearest page size
    page_table_memory = (page_table_memory + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    serial::com1_printf("Total memory required for page tables: %llu KB\n", page_table_memory / 1024);

    // Access largest conventional memory segment
    // The segment has to be large enough to fit the bitmap and the full page table covering system memory
    efi::efi_memory_descriptor_wrapper largest_segment = memory_map.find_segment_for_allocation_block(
        10 * (1 << 20),     // Start searching from the 10MB point to be guaranteed above the kernel
        1ULL << 30,         // Pick the largest segment within the 1GB range
        page_bitmap_size + page_table_memory
    );

    if (largest_segment.desc == nullptr) {
        serial::com1_printf("[*] No conventional memory segments found!\n");
        return;
    }
    
    uint64_t physical_start = largest_segment.paddr;
    uint64_t length = largest_segment.length;

    serial::com1_printf(
        "Largest Conventional Memory Segment:\n"
        "  Size: %llu MB (%llu pages)\n"
        "  Physical: 0x%016llx - 0x%016llx\n"
        "  Virtual:  0x%016llx - 0x%016llx\n",
        length / (1024 * 1024), length / 4096,
        physical_start, physical_start + length,
        physical_start + 0xffffff8000000000, physical_start + length + 0xffffff8000000000);

    // Initialize the bootstrap allocator
    auto& bootstrap_allocator = allocators::page_bootstrap_allocator::get();
    bootstrap_allocator.init(physical_start + page_bitmap_size, page_table_memory);

    // Create a new top level page table
    page_table* new_pml4 = (page_table*)bootstrap_allocator.alloc_physical_page();

    for (const auto& entry : memory_map) {
        // Filter for EfiConventionalMemory (type 7)
        if (entry.desc->type != 7) {
            continue;
        }

        uint64_t physical_start = entry.paddr;
        uint64_t length = entry.length;

        for (uint64_t vaddr = physical_start; vaddr < physical_start + length; vaddr += PAGE_SIZE) {
            map_page(vaddr, vaddr, new_pml4, bootstrap_allocator);
        }
    }

    for (uint64_t vaddr = 0xffffffff80000000; vaddr < 0xffffffff80000000 + 4 * 1024 * 1024; vaddr += PAGE_SIZE) {
        uintptr_t paddr = vaddr - 0xffffffff80000000;
        map_page(vaddr, paddr, new_pml4, bootstrap_allocator);
    }

    // Install the new page table
    asm volatile(
        "mov %0, %%cr3"  // Move the value into the CR3 register
        :
        : "r"(new_pml4)  // Input operand: new_pml4
        : "memory"       // Clobber: memory to indicate the register affects memory
    );

    serial::com1_printf("new page table installed: 0x%llx\n", (uintptr_t)new_pml4);

    page_frame_bitmap::get().init(page_bitmap_size, (uint8_t*)(physical_start));
    serial::com1_printf("\n[*] Page bitmap initialized!\n");
}
} // namespace paging

