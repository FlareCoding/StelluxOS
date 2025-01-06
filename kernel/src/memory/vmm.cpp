#include <memory/vmm.h>
#include <memory/paging.h>
#include <memory/allocators/page_bitmap_allocator.h>
#include <sync.h>

namespace vmm {
// Global mutex  for virtual memory manager
DECLARE_GLOBAL_OBJECT(mutex, vmm_lock);

// Allocates a single virtual page and maps it to a new physical page
__PRIVILEGED_CODE
void* alloc_virtual_page(uint64_t flags) {
    mutex_guard guard(vmm_lock);

    void* phys_page = allocators::page_bitmap_allocator::get_physical_allocator().alloc_page();
    if (!phys_page) {
        return nullptr; // Physical page allocation failed
    }

    void* virt_page = allocators::page_bitmap_allocator::get_virtual_allocator().alloc_page();
    if (!virt_page) {
        allocators::page_bitmap_allocator::get_physical_allocator().free_page(phys_page);
        return nullptr; // Virtual page allocation failed
    }

    paging::map_page(
        reinterpret_cast<uintptr_t>(virt_page),
        reinterpret_cast<uintptr_t>(phys_page),
        flags,
        paging::get_pml4()
    );

    return virt_page;
}

// Maps an existing physical page to a virtual page
__PRIVILEGED_CODE
void* map_physical_page(uintptr_t paddr, uint64_t flags) {
    mutex_guard guard(vmm_lock);

    void* virt_page = allocators::page_bitmap_allocator::get_virtual_allocator().alloc_page();
    if (!virt_page) {
        return nullptr; // Virtual page allocation failed
    }

    paging::map_page(reinterpret_cast<uintptr_t>(virt_page), paddr, flags, paging::get_pml4());
    return virt_page;
}

// Allocates and maps a range of contiguous virtual pages to new physical pages
__PRIVILEGED_CODE
void* alloc_virtual_pages(size_t count, uint64_t flags) {
    mutex_guard guard(vmm_lock);

    void* virt_start = allocators::page_bitmap_allocator::get_virtual_allocator().alloc_pages(count);
    if (!virt_start) {
        return nullptr; // Virtual pages allocation failed
    }

    for (size_t i = 0; i < count; ++i) {
        void* phys_page = allocators::page_bitmap_allocator::get_physical_allocator().alloc_page();
        if (!phys_page) {
            // Free previously allocated physical pages
            unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(virt_start), i);
            return nullptr;
        }

        paging::map_page(
            reinterpret_cast<uintptr_t>(virt_start) + i * PAGE_SIZE, 
            reinterpret_cast<uintptr_t>(phys_page), 
            flags,
            paging::get_pml4()
        );
    }

    return virt_start;
}

// Allocates a contiguous range of virtual pages and maps them to contiguous physical pages
__PRIVILEGED_CODE
void* alloc_contiguous_virtual_pages(size_t count, uint64_t flags) {
    mutex_guard guard(vmm_lock);

    void* phys_start = allocators::page_bitmap_allocator::get_physical_allocator().alloc_pages(count);
    if (!phys_start) {
        return nullptr; // Contiguous physical pages allocation failed
    }

    void* virt_start = allocators::page_bitmap_allocator::get_virtual_allocator().alloc_pages(count);
    if (!virt_start) {
        allocators::page_bitmap_allocator::get_physical_allocator().free_pages(phys_start, count);
        return nullptr; // Contiguous virtual pages allocation failed
    }

    paging::map_pages(
        reinterpret_cast<uintptr_t>(virt_start), 
        reinterpret_cast<uintptr_t>(phys_start), 
        count,
        flags,
        paging::get_pml4()
    );

    return virt_start;
}

// Maps a contiguous range of physical pages to virtual pages
__PRIVILEGED_CODE
void* map_contiguous_physical_pages(uintptr_t paddr, size_t count, uint64_t flags) {
    mutex_guard guard(vmm_lock);

    void* virt_start = allocators::page_bitmap_allocator::get_virtual_allocator().alloc_pages(count);
    if (!virt_start) {
        return nullptr; // Contiguous virtual pages allocation failed
    }

    paging::map_pages(
        reinterpret_cast<uintptr_t>(virt_start),
        paddr,
        count,
        flags,
        paging::get_pml4()
    );
    
    return virt_start;
}

__PRIVILEGED_CODE
void* alloc_linear_mapped_persistent_page() {
    mutex_guard guard(vmm_lock);

    void* phys_start = allocators::page_bitmap_allocator::get_physical_allocator().alloc_page();
    if (!phys_start) {
        return nullptr;
    }

    void* virt_start = paging::phys_to_virt_linear(phys_start);
    return virt_start;
}

__PRIVILEGED_CODE
void* alloc_linear_mapped_persistent_pages(size_t count) {
    mutex_guard guard(vmm_lock);

    void* phys_start = allocators::page_bitmap_allocator::get_physical_allocator().alloc_pages(count);
    if (!phys_start) {
        return nullptr;
    }

    void* virt_start = paging::phys_to_virt_linear(phys_start);
    return virt_start;
}

// Unmaps a single virtual page
__PRIVILEGED_CODE
void unmap_virtual_page(uintptr_t vaddr) {
    mutex_guard guard(vmm_lock);

    uintptr_t paddr = paging::get_physical_address(reinterpret_cast<void*>(vaddr));
    if (paddr) {
        allocators::page_bitmap_allocator::get_physical_allocator().free_page(reinterpret_cast<void*>(paddr));
    }

    paging::map_page(vaddr, 0, 0, paging::get_pml4()); // Unmap the page by clearing the entry
    allocators::page_bitmap_allocator::get_virtual_allocator().free_page(reinterpret_cast<void*>(vaddr));
}

// Unmaps a contiguous range of virtual pages
__PRIVILEGED_CODE
void unmap_contiguous_virtual_pages(uintptr_t vaddr, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        unmap_virtual_page(vaddr + i * PAGE_SIZE);
    }
}
} // namespace vmm
