#include <syscall/handlers/sys_mem.h>
#include <memory/paging.h>
#include <memory/memory.h>
#include <memory/allocators/page_bitmap_allocator.h>
#include <memory/vmm.h>
#include <process/vma.h>
#include <process/mm.h>
#include <process/process.h>
#include <core/klog.h>

DECLARE_SYSCALL_HANDLER(mmap) {
    uintptr_t addr = static_cast<uintptr_t>(arg1);
    size_t length = static_cast<size_t>(arg2);
    int prot = static_cast<int>(arg3);
    int flags = static_cast<int>(arg4);
    int fd = static_cast<int>(arg5);
    size_t offset = static_cast<size_t>(arg6);

    __unused fd;
    
    SYSCALL_TRACE("mmap(0x%llx, %llu, %i, %i, %i, %lli) = ", addr, length, prot, flags, fd, offset);

    // Parameter validation
    
    // Validate length
    if (length == 0) {
        SYSCALL_TRACE("-EINVAL (length is 0)\n");
        return -EINVAL;
    }
    
    // Check for length overflow when page-aligned
    if (length > SIZE_MAX - PAGE_SIZE + 1) {
        SYSCALL_TRACE("-ENOMEM (length too large)\n");
        return -ENOMEM;
    }
    
    // Validate protection flags
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
        SYSCALL_TRACE("-EINVAL (invalid prot flags)\n");
        return -EINVAL;
    }
    
    // Validate mapping flags
    const int valid_flags = MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS;
    if (flags & ~valid_flags) {
        SYSCALL_TRACE("-EINVAL (invalid map flags)\n");
        return -EINVAL;
    }
    
    // Must specify either MAP_SHARED or MAP_PRIVATE, but not both
    if (!(flags & (MAP_SHARED | MAP_PRIVATE)) || 
        ((flags & MAP_SHARED) && (flags & MAP_PRIVATE))) {
        SYSCALL_TRACE("-EINVAL (must specify exactly one of MAP_SHARED or MAP_PRIVATE)\n");
        return -EINVAL;
    }
    
    // For now, only support anonymous mappings
    if (!(flags & MAP_ANONYMOUS)) {
        SYSCALL_TRACE("-ENOSYS (file-backed mappings not yet supported)\n");
        return -ENOSYS;
    }
    
    // Validate address alignment if specified
    if (addr && (addr & (PAGE_SIZE - 1))) {
        SYSCALL_TRACE("-EINVAL (address not page-aligned)\n");
        return -EINVAL;
    }
    
    // For anonymous mappings, offset should be 0
    if ((flags & MAP_ANONYMOUS) && offset != 0) {
        SYSCALL_TRACE("-EINVAL (offset must be 0 for anonymous mappings)\n");
        return -EINVAL;
    }
    
    // Page-align length (round up)
    length = PAGE_ALIGN_UP(length);
    
    // Find virtual address
    uintptr_t target_addr;
    mm_context* mm_ctx = &current->get_core()->mm_ctx;
    
    if (flags & MAP_FIXED) {
        // User demands specific address
        if (!addr) {
            SYSCALL_TRACE("-EINVAL (MAP_FIXED requires non-null address)\n");
            return -EINVAL;
        }
        target_addr = addr;
        
        // For MAP_FIXED, Linux behavior: automatically unmap any existing mappings in this range
        // Find all VMAs that overlap with [target_addr, target_addr + length)
        uintptr_t range_end = target_addr + length;
        vma_area* vma = mm_ctx->vma_list;
        
        while (vma) {
            vma_area* next_vma = vma->next;  // Save next before potential deletion
            
            // Check if this VMA overlaps with our target range
            if (vma->start < range_end && vma->end > target_addr) {
                uintptr_t overlap_start = vma->start > target_addr ? vma->start : target_addr;
                uintptr_t overlap_end = vma->end < range_end ? vma->end : range_end;
                
                // Unmap physical pages in the overlapping region
                paging::page_table* pt = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);
                auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
                
                for (uintptr_t page_addr = PAGE_ALIGN_DOWN(overlap_start); 
                     page_addr < PAGE_ALIGN_UP(overlap_end); 
                     page_addr += PAGE_SIZE) {
                    
                    void* phys_addr = reinterpret_cast<void*>(
                        paging::get_physical_address(reinterpret_cast<void*>(page_addr), pt)
                    );
                    if (phys_addr) {
                        paging::unmap_page(page_addr, pt);
                        physalloc.free_page(phys_addr);
                    }
                }
                
                // Handle VMA splitting/removal based on overlap type
                if (vma->start >= target_addr && vma->end <= range_end) {
                    // VMA is completely within target range - remove it entirely
                    remove_vma(mm_ctx, vma);
                } else if (vma->start < target_addr && vma->end > range_end) {
                    // VMA completely contains target range - split into two parts
                    // First, split at range_end to create the second part
                    vma_area* second_part = split_vma(mm_ctx, vma, range_end);
                    if (second_part) {
                        // Now split the first part at target_addr
                        split_vma(mm_ctx, vma, target_addr);
                        // The middle part (between target_addr and range_end) is now a separate VMA
                        // Find and remove it
                        vma_area* middle_part = find_vma(mm_ctx, target_addr);
                        if (middle_part) {
                            remove_vma(mm_ctx, middle_part);
                        }
                    }
                } else if (vma->start < target_addr && vma->end > target_addr) {
                    // VMA overlaps start of target range - truncate it
                    vma->end = target_addr;
                } else if (vma->start < range_end && vma->end > range_end) {
                    // VMA overlaps end of target range - truncate from start
                    vma->start = range_end;
                }
            }
            
            vma = next_vma;
        }
    } else {
        // Kernel chooses address
        target_addr = find_free_vma_range(mm_ctx, length, 0, addr);
        if (!target_addr) {
            SYSCALL_TRACE("-ENOMEM (no free address range found)\n");
            return -ENOMEM;
        }
    }
    
    // Create the VMA
    uint64_t vma_prot = prot;
    
    uint64_t vma_type = VMA_TYPE_ANONYMOUS;
    if (flags & MAP_SHARED) {
        vma_type |= VMA_TYPE_SHARED;
    } else {
        vma_type |= VMA_TYPE_PRIVATE;
    }
    
    vma_area* vma = create_vma(mm_ctx, target_addr, length, vma_prot, vma_type);
    if (!vma) {
        SYSCALL_TRACE("-ENOMEM (failed to create VMA)\n");
        return -ENOMEM;
    }
    
    // Allocate and map physical memory
    // For now, use eager allocation (allocate all pages immediately)
    paging::page_table* page_table = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);
    auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
    
    size_t num_pages = length / PAGE_SIZE;
    
    // Convert protection flags to page table flags
    uint64_t page_flags = PTE_PRESENT | PTE_US;  // User accessible
    if (prot & PROT_WRITE) {
        page_flags |= PTE_RW;
    }
    if (!(prot & PROT_EXEC)) {
        page_flags |= PTE_NX;
    }
    
    // Allocate and map pages one by one to avoid needing contiguous physical memory
    bool allocation_failed = false;
    size_t pages_allocated = 0;
    
    for (size_t i = 0; i < num_pages; i++) {
        uintptr_t virt_addr = target_addr + (i * PAGE_SIZE);
        
        void* phys_page = physalloc.alloc_page();
        if (!phys_page) {
            allocation_failed = true;
            break;
        }
        
        // Map the page
        paging::map_page(virt_addr, reinterpret_cast<uintptr_t>(phys_page), page_flags, page_table);
        
        // Zero out the page for security
        void* virt_page = paging::phys_to_virt_linear(phys_page);
        if (virt_page) {
            memset(virt_page, 0, PAGE_SIZE);
        }
        
        pages_allocated++;
    }
    
    // Handle allocation failure
    if (allocation_failed) {
        // Clean up: unmap and free allocated pages
        for (size_t i = 0; i < pages_allocated; i++) {
            uintptr_t virt_addr = target_addr + (i * PAGE_SIZE);
            void* phys_addr = reinterpret_cast<void*>(
                paging::get_physical_address(reinterpret_cast<void*>(virt_addr), page_table)
            );
            if (phys_addr) {
                paging::unmap_page(virt_addr, page_table);
                physalloc.free_page(phys_addr);
            }
        }
        
        // Remove the VMA
        remove_vma(mm_ctx, vma);
        
        SYSCALL_TRACE("-ENOMEM (failed to allocate physical pages)\n");
        return -ENOMEM;
    }
    
    // Success!
    SYSCALL_TRACE("0x%llx\n", target_addr);
    return static_cast<long>(target_addr);
}

DECLARE_SYSCALL_HANDLER(munmap) {
    uintptr_t addr = static_cast<uintptr_t>(arg1);
    size_t length = static_cast<size_t>(arg2);
    
    SYSCALL_TRACE("munmap(0x%llx, %llu) = ", addr, length);

    // Parameter validation
    
    // Check for null address
    if (!addr) {
        SYSCALL_TRACE("-EINVAL (null address)\n");
        return -EINVAL;
    }
    
    // Check for zero length
    if (length == 0) {
        SYSCALL_TRACE("-EINVAL (zero length)\n");
        return -EINVAL;
    }
    
    // Check address alignment (must be page-aligned)
    if (addr & (PAGE_SIZE - 1)) {
        SYSCALL_TRACE("-EINVAL (address not page-aligned)\n");
        return -EINVAL;
    }
    
    // Check for length overflow when page-aligned
    if (length > SIZE_MAX - PAGE_SIZE + 1) {
        SYSCALL_TRACE("-EINVAL (length too large)\n");
        return -EINVAL;
    }
    
    // Page-align length (round up)
    length = PAGE_ALIGN_UP(length);
    
    // Validate address range is within user space
    if (addr < USERSPACE_START || addr > USERSPACE_END - length) {
        SYSCALL_TRACE("-EINVAL (address range outside user space)\n");
        return -EINVAL;
    }
    
    mm_context* mm_ctx = &current->get_core()->mm_ctx;

    paging::page_table* page_table = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);
    auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
    
    uintptr_t range_end = addr + length;
    
    // Find all VMAs that overlap with [addr, addr + length) and handle them
    vma_area* vma = mm_ctx->vma_list;
    
    while (vma) {
        vma_area* next_vma = vma->next;  // Save next before potential deletion/modification
        
        // Check if this VMA overlaps with our target range
        if (vma->start < range_end && vma->end > addr) {
            uintptr_t overlap_start = vma->start > addr ? vma->start : addr;
            uintptr_t overlap_end = vma->end < range_end ? vma->end : range_end;
            
            // Unmap physical pages in the overlapping region
            for (uintptr_t page_addr = PAGE_ALIGN_DOWN(overlap_start); 
                 page_addr < PAGE_ALIGN_UP(overlap_end); 
                 page_addr += PAGE_SIZE) {
                
                void* phys_addr = reinterpret_cast<void*>(
                    paging::get_physical_address(reinterpret_cast<void*>(page_addr), page_table)
                );
                if (phys_addr) {
                    paging::unmap_page(page_addr, page_table);
                    physalloc.free_page(phys_addr);
                }
                // Note: We gracefully handle pages that aren't mapped (Linux behavior)
            }
            
            // Handle VMA modification based on overlap type
            if (vma->start >= addr && vma->end <= range_end) {
                // Case 1: VMA is completely within munmap range - remove it entirely
                remove_vma(mm_ctx, vma);
            } else if (vma->start < addr && vma->end > range_end) {
                // Case 4: VMA completely contains munmap range - split into two parts
                // First, split at range_end to create the second part
                vma_area* second_part = split_vma(mm_ctx, vma, range_end);
                if (second_part) {
                    // Now split the first part at addr
                    vma_area* middle_part = split_vma(mm_ctx, vma, addr);
                    // The middle part (between addr and range_end) needs to be removed
                    if (middle_part) {
                        remove_vma(mm_ctx, middle_part);
                    }
                }
            } else if (vma->start < addr && vma->end > addr) {
                // Case 2: VMA overlaps start of munmap range - shrink from end
                vma->end = addr;
            } else if (vma->start < range_end && vma->end > range_end) {
                // Case 3: VMA overlaps end of munmap range - shrink from start
                // Adjust file offset if this is a file-backed mapping
                if (vma->file_backing) {
                    vma->file_offset += (range_end - vma->start);
                }
                vma->start = range_end;
            }
            
            // Try to merge with adjacent VMAs after modification (optimization)
            if (vma->start < addr || vma->end > range_end) {
                // Only try to merge if the VMA still exists and wasn't completely removed
                merge_vmas(mm_ctx, vma);
            }
        }
        
        vma = next_vma;
    }
    
    // Success!
    SYSCALL_TRACE("munmap(0x%llx, %llu) = 0\n", addr, length);
    return 0;
}

DECLARE_SYSCALL_HANDLER(brk) {
    uintptr_t new_heap_end = static_cast<uintptr_t>(arg1);
    
    // If arg1 is 0, just return the current heap end
    if (new_heap_end == 0) {
        long result = static_cast<long>(current->get_core()->mm_ctx.heap_end);
        SYSCALL_TRACE("brk(0x%llx) = 0x%llx\n", arg1, result);
        return result;
    } else {
        // Try to set the new heap end
        long result;
        if (manage_process_heap(&current->get_core()->mm_ctx, new_heap_end)) {
            result = static_cast<long>(current->get_core()->mm_ctx.heap_end);
        } else {
            // On failure, return the current heap end (Linux behavior)
            result = static_cast<long>(current->get_core()->mm_ctx.heap_end);
        }
        SYSCALL_TRACE("brk(0x%llx) = 0x%llx\n", arg1, result);
        return result;
    }
} 