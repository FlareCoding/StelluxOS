#include <memory/paging.h>
#include <memory/memory.h>
#include <serial/serial.h>
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

__PRIVILEGED_DATA uint64_t page_frame_bitmap::_size;
__PRIVILEGED_DATA uint8_t* page_frame_bitmap::_buffer;
__PRIVILEGED_DATA uint64_t page_frame_bitmap::_next_free_index;

__PRIVILEGED_CODE
page_frame_bitmap& page_frame_bitmap::get() {
    __PRIVILEGED_DATA
    static page_frame_bitmap g_page_frame_bitmap;

    return g_page_frame_bitmap;
}

__PRIVILEGED_CODE
void page_frame_bitmap::init(uint64_t size, uint8_t* buffer) {
    _size = size;
    _buffer = buffer;
    _next_free_index = 0;

    zeromem(buffer, size);
}

__PRIVILEGED_CODE
uint64_t page_frame_bitmap::get_size() const {
    return _size;
}

__PRIVILEGED_CODE
uint64_t page_frame_bitmap::get_next_free_index() const {
    return _next_free_index;
}

__PRIVILEGED_CODE
void page_frame_bitmap::set_next_free_index(uint64_t idx) const {
    _next_free_index = idx;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_page_free(void* paddr) {
    bool result = _set_page_value(paddr, false);
    if (result) {
        uint64_t index = _get_addr_index(paddr);
        if (index < _next_free_index) {
            _next_free_index = index; // Update next free index
        }
    }
    return result;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_page_used(void* paddr) {
    bool result = _set_page_value(paddr, true);
    if (result) {
        uint64_t index = _get_addr_index(paddr);
        if (index == _next_free_index) {
            _next_free_index = index + 1; // Move to the next index
        }
    }
    return result;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_pages_free(void* paddr, size_t count) {
    uint64_t start_index = _get_addr_index(paddr);

    // Check that we don't go beyond the bitmap buffer
    if ((start_index + count) > (_size * 8))
        return false;

    for (size_t i = 0; i < count; ++i) {
        void* addr = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(paddr) + i * PAGE_SIZE);
        if (!_set_page_value(addr, false))
            return false;
    }

    if (start_index < _next_free_index) {
        _next_free_index = start_index; // Update next free index
    }

    return true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_pages_used(void* paddr, size_t count) {
    uint64_t start_index = _get_addr_index(paddr);

    // Check that we don't go beyond the bitmap buffer
    if ((start_index + count) > (_size * 8))
        return false;

    for (size_t i = 0; i < count; ++i) {
        void* addr = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(paddr) + i * PAGE_SIZE);
        if (!_set_page_value(addr, true))
            return false;
    }

    if (start_index <= _next_free_index) {
        _next_free_index = start_index + count; // Update next free index
    }

    return true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::is_page_free(void* paddr) {
    return (_get_page_value(paddr) == false);
}

__PRIVILEGED_CODE
bool page_frame_bitmap::is_page_used(void* paddr) {
    return (_get_page_value(paddr) == true);
}

__PRIVILEGED_CODE
bool page_frame_bitmap::_set_page_value(void* paddr, bool value) {
    uint64_t index = _get_addr_index(paddr);

    // Preventing bitmap buffer overflow
    if (index > (_size * 8))
        return false;

    uint64_t byte_idx = index / 8;
    uint8_t bit_idx = index % 8;
    uint8_t mask = 0b00000001 << bit_idx;

    // First disable the bit
    _buffer[byte_idx] &= ~mask;

    // Now enable the bit if needed
    if (value) {
        _buffer[byte_idx] |= mask;
    }

    return true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::_get_page_value(void* paddr) {
    uint64_t index = _get_addr_index(paddr);
    uint64_t byte_idx = index / 8;
    uint8_t bit_idx = index % 8;
    uint8_t mask = 0b00000001 << bit_idx;

    return (_buffer[byte_idx] & mask) > 0;
}

__PRIVILEGED_CODE
uint64_t page_frame_bitmap::_get_addr_index(void* paddr) {
    return reinterpret_cast<uint64_t>(paddr) / PAGE_SIZE;
}

__PRIVILEGED_CODE
void lock_physical_page(void* paddr) {
    // Align the physical address to the page boundary
    uintptr_t addr = reinterpret_cast<uintptr_t>(paddr);
    addr &= ~(PAGE_SIZE - 1);
    void* aligned_paddr = reinterpret_cast<void*>(addr);
    
    // Mark the page as used (locked)
    bool success = page_frame_bitmap::get().mark_page_used(aligned_paddr);
    if (!success) {
        serial::com1_printf("[*] failed to lock physical page: 0x%016llx\n", aligned_paddr);
    }
}

__PRIVILEGED_CODE
void lock_physical_pages(void* paddr, size_t count) {
    // Align the physical address to the page boundary
    uintptr_t addr = reinterpret_cast<uintptr_t>(paddr);
    addr &= ~(PAGE_SIZE - 1);
    void* aligned_paddr = reinterpret_cast<void*>(addr);
    
    // Mark the pages as used (locked)
    bool success = page_frame_bitmap::get().mark_pages_used(aligned_paddr, count);
    if (!success) {
        serial::com1_printf("[*] failed to lock %u physical pages at: 0x%016llx\n", count, aligned_paddr);
    }
}

__PRIVILEGED_CODE
void free_physical_page(void* paddr) {
    // Align the physical address to the page boundary
    uintptr_t addr = reinterpret_cast<uintptr_t>(paddr);
    addr &= ~(PAGE_SIZE - 1);
    void* aligned_paddr = reinterpret_cast<void*>(addr);
    
    // Mark the page as free
    bool success = page_frame_bitmap::get().mark_page_free(aligned_paddr);
    if (!success) {
        serial::com1_printf("[*] failed to free physical page: 0x%016llx\n", aligned_paddr);
    }
}

__PRIVILEGED_CODE
void free_physical_pages(void* paddr, size_t count) {
    // Align the physical address to the page boundary
    uintptr_t addr = reinterpret_cast<uintptr_t>(paddr);
    addr &= ~(PAGE_SIZE - 1);
    void* aligned_paddr = reinterpret_cast<void*>(addr);
    
    // Mark the pages as free
    bool success = page_frame_bitmap::get().mark_pages_free(aligned_paddr, count);
    if (!success) {
        serial::com1_printf("[*] failed to free %u physical pages at: 0x%016llx\n", count, aligned_paddr);
    }
}

__PRIVILEGED_CODE
void* alloc_physical_page() {
    page_frame_bitmap& bitmap = page_frame_bitmap::get();
    uint64_t size = bitmap.get_size() * 8; // Total number of pages

    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = size;

    // Search from _next_free_index to the end
    for (; index < end_index; ++index) {
        uintptr_t paddr = index * PAGE_SIZE;
        void* addr = reinterpret_cast<void*>(paddr);

        if (bitmap.is_page_free(addr)) {
            if (bitmap.mark_page_used(addr)) {
                bitmap.set_next_free_index(index + 1);
                return addr;
            }
        }
    }

    // No free page found
    return nullptr;
}

__PRIVILEGED_CODE
void* alloc_physical_pages(size_t count) {
    if (count == 0) {
        return nullptr;
    }

    page_frame_bitmap& bitmap = page_frame_bitmap::get();
    uint64_t size = bitmap.get_size() * 8; // Total number of pages

    size_t consecutive = 0;
    uint64_t start_index = 0;
    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = size;

    // Search from _next_free_index to the end
    for (; index < end_index; ++index) {
        void* addr = reinterpret_cast<void*>(index * PAGE_SIZE);

        if (bitmap.is_page_free(addr)) {
            if (consecutive == 0) {
                start_index = index;
            }
            consecutive++;

            if (consecutive == count) {
                void* start_addr = reinterpret_cast<void*>(start_index * PAGE_SIZE);
                if (bitmap.mark_pages_used(start_addr, count)) {
                    bitmap.set_next_free_index(start_index + count);
                    return start_addr;
                }
                consecutive = 0;
            }
        } else {
            consecutive = 0;
        }
    }

    // No suitable contiguous block found
    return nullptr;
}

__PRIVILEGED_CODE
void* alloc_physical_pages_aligned(size_t count, uint64_t alignment) {
    if (count == 0 || (alignment & (alignment - 1)) != 0) {
        // Alignment is not a power of two or count is zero
        return nullptr;
    }

    page_frame_bitmap& bitmap = page_frame_bitmap::get();
    uint64_t size = bitmap.get_size() * 8; // Total number of pages

    // Calculate the alignment in terms of pages
    if (alignment < PAGE_SIZE) {
        alignment = PAGE_SIZE;
    }
    uint64_t alignment_pages = alignment / PAGE_SIZE;

    uint64_t index = bitmap.get_next_free_index();
    uint64_t end_index = size;

    // Search from _next_free_index to the end
    for (; index < end_index; ++index) {
        // Check alignment
        if ((index % alignment_pages) != 0) {
            continue;
        }

        // Check if there are enough pages remaining
        if (index + count > size) {
            break;
        }

        bool block_free = true;
        for (uint64_t offset = 0; offset < count; ++offset) {
            void* current_addr = reinterpret_cast<void*>((index + offset) * PAGE_SIZE);
            if (!bitmap.is_page_free(current_addr)) {
                block_free = false;
                break;
            }
        }

        if (block_free) {
            void* start_addr = reinterpret_cast<void*>(index * PAGE_SIZE);
            if (bitmap.mark_pages_used(start_addr, count)) {
                bitmap.set_next_free_index(index + count);
                return start_addr;
            }
        }
    }

    // No suitable aligned contiguous block found
    return nullptr;
}

// __PRIVILEGED_CODE
// void map_virtual_page(
//     void* vaddr,
//     void* paddr
// ) {
//     // Retrieve the current PML4 from CR3
//     uint64_t cr3;
//     asm volatile ("mov %%cr3, %0" : "=r"(cr3));
//     page_table* pml4 = (page_table*)cr3;

//     virt_addr_indices_t vaddr_indices =
//         get_vaddr_page_table_indices(reinterpret_cast<uintptr_t>(vaddr));

// 	page_table *pdpt = nullptr, *pdt = nullptr, *pt = nullptr;

// 	pte_t* pml4_entry = &pml4->entries[vaddr_indices.pml4];

// 	if (pml4_entry->present == 0) {
// 		pdpt = (page_table*)(g_available_page);
//         g_available_page += PAGE_SIZE;

// 		pml4_entry->present = 1;
// 		pml4_entry->read_write = 1;
// 		pml4_entry->page_frame_number = (reinterpret_cast<uint64_t>(pdpt) - 0xffffffff80000000) >> 12;
// 	} else {
// 		pdpt = (page_table*)(((uint64_t)pml4_entry->page_frame_number << 12) + 0xffffffff80000000);
// 	}

// 	pte_t* pdpt_entry = &pdpt->entries[vaddr_indices.pdpt];
	
// 	if (pdpt_entry->present == 0) {
// 		pdt = (page_table*)(g_available_page);
//         g_available_page += PAGE_SIZE;

// 		pdpt_entry->present = 1;
// 		pdpt_entry->read_write = 1;
// 		pdpt_entry->page_frame_number = (reinterpret_cast<uint64_t>(pdt) - 0xffffffff80000000) >> 12;
// 	} else {
// 		pdt = (page_table*)(((uint64_t)pdpt_entry->page_frame_number << 12) + 0xffffffff80000000);
// 	}

// 	pte_t* pdt_entry = &pdt->entries[vaddr_indices.pdt];
	
// 	if (pdt_entry->present == 0) {
// 		pt = (page_table*)(g_available_page);
//         g_available_page += PAGE_SIZE;

// 		pdt_entry->present = 1;
// 		pdt_entry->read_write = 1;
// 		pdt_entry->page_frame_number = (reinterpret_cast<uint64_t>(pt) - 0xffffffff80000000) >> 12;
// 	} else {
// 		pt = (page_table*)(((uint64_t)pdt_entry->page_frame_number << 12) + 0xffffffff80000000);
// 	}

// 	pte_t* pte = &pt->entries[vaddr_indices.pt];
// 	pte->present = 1;
// 	pte->read_write = 1;
// 	pte->page_frame_number = reinterpret_cast<uint64_t>(paddr) >> 12;
// }

// void map_2mb_page(void* vaddr, void* paddr) {
//     uint64_t cr3;
//     asm volatile ("mov %%cr3, %0" : "=r"(cr3));
//     page_table* pml4 = (page_table*)cr3;

//     virt_addr_indices_t vaddr_indices =
//         get_vaddr_page_table_indices(reinterpret_cast<uintptr_t>(vaddr));

//     page_table *pdpt = nullptr, *pdt = nullptr;

//     pte_t* pml4_entry = &pml4->entries[vaddr_indices.pml4];

//     if (pml4_entry->present == 0) {
//         pdpt = (page_table*)(g_available_page);
//         g_available_page += PAGE_SIZE;

//         pml4_entry->present = 1;
//         pml4_entry->read_write = 1;
//         pml4_entry->page_frame_number = (reinterpret_cast<uint64_t>(pdpt) - 0xffffffff80000000) >> 12;
//     } else {
//         pdpt = (page_table*)(((uint64_t)pml4_entry->page_frame_number << 12) + 0xffffffff80000000);
//     }

//     pte_t* pdpt_entry = &pdpt->entries[vaddr_indices.pdpt];

//     if (pdpt_entry->present == 0) {
//         pdt = (page_table*)(g_available_page);
//         g_available_page += PAGE_SIZE;

//         pdpt_entry->present = 1;
//         pdpt_entry->read_write = 1;
//         pdpt_entry->page_frame_number = (reinterpret_cast<uint64_t>(pdt) - 0xffffffff80000000) >> 12;
//     } else {
//         pdt = (page_table*)(((uint64_t)pdpt_entry->page_frame_number << 12) + 0xffffffff80000000);
//     }

//     pde_t* pdt_entry = (pde_t*)&pdt->entries[vaddr_indices.pdt];
//     pdt_entry->present = 1;
//     pdt_entry->read_write = 1;
//     pdt_entry->page_size = 1; // Large page (2MB)
//     pdt_entry->page_frame_number = reinterpret_cast<uint64_t>(paddr) >> 21;
// }

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

    // Calculate the size needed for the page frame bitmap
    uint64_t page_bitmap_size = memory_map.get_total_conventional_memory() / PAGE_SIZE / 8 + 1;
    serial::com1_printf("\nPage Bitmap Size: %llu KB\n", page_bitmap_size / 1024);

     uint64_t page_table_memory = calculate_page_table_memory(memory_map.get_total_system_memory());
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

    uint64_t kernel_start = (uint64_t)&__ksymstart;
    uint64_t kernel_end = (uint64_t)&__ksymend;
    serial::com1_printf("Kernel Size: %llu KB\n    ", (kernel_end - kernel_start) / 1024);
    serial::com1_printf(
        "0x%016llx-0x%016llx (%d pages)\n",
        kernel_start - 0xffffffff80000000, kernel_end - 0xffffffff80000000,
        (kernel_end - kernel_start) / PAGE_SIZE
    );

    page_frame_bitmap::get().init(page_bitmap_size, (uint8_t*)(physical_start));
    serial::com1_printf("\n[*] Page bitmap initialized!\n");
}
} // namespace paging

