#include <process/vma.h>
#include <memory/paging.h>
#include <memory/vmm.h>
#include <core/klog.h>

__PRIVILEGED_CODE
bool init_process_vma(mm_context* mm_ctx) {
    if (!mm_ctx) {
        return false;
    }

    // Initialize VMA management
    mm_ctx->vma_list = nullptr;
    mm_ctx->vma_count = 0;
    
    // Set mmap_base to start of mmap region (grows upward from here)
    mm_ctx->mmap_base = MMAP_REGION_START;
    mm_ctx->task_size = USERSPACE_END - USERSPACE_START;

    return true;
}

/**
 * @brief Check if two address ranges overlap
 * @param start1 Start of first range
 * @param end1 End of first range (exclusive)
 * @param start2 Start of second range  
 * @param end2 End of second range (exclusive)
 * @return true if ranges overlap
 */
static bool ranges_overlap(uintptr_t start1, uintptr_t end1, uintptr_t start2, uintptr_t end2) {
    return (start1 < end2) && (start2 < end1);
}

/**
 * @brief Check if an address range conflicts with any existing VMA
 * @param mm_ctx Memory management context
 * @param start Start address of range to check
 * @param size Size of range to check
 * @return Pointer to conflicting VMA, or nullptr if no conflict
 */
static vma_area* find_vma_conflict(mm_context* mm_ctx, uintptr_t start, size_t size) {
    if (!mm_ctx) {
        return nullptr;
    }
    
    uintptr_t end = start + size;
    
    vma_area* vma = mm_ctx->vma_list;
    while (vma) {
        if (ranges_overlap(start, end, vma->start, vma->end)) {
            return vma;
        }
        vma = vma->next;
    }
    
    return nullptr;
}

/**
 * @brief Find a free address range in the mmap region
 * @param mm_ctx Memory management context
 * @param size Size of range needed
 * @return Start address of free range, or 0 if none found
 * 
 * This implements a simple growing-upward allocation strategy:
 * 1. Start from mmap_base
 * 2. Find the first gap large enough for the allocation
 * 3. Return the start of that gap
 */
static uintptr_t find_free_mmap_range(mm_context* mm_ctx, size_t size) {
    if (!mm_ctx || size == 0) {
        return 0;
    }
    
    // Start searching from mmap_base
    uintptr_t search_addr = mm_ctx->mmap_base;
    const uintptr_t search_end = STACK_REGION_START;
    
    while (search_addr + size <= search_end) {
        // Check if this range conflicts with any existing VMA
        if (!find_vma_conflict(mm_ctx, search_addr, size)) {
            return search_addr;  // Found a free range
        }
        
        // Find the next potential start address by moving past the conflicting VMA
        vma_area* conflict = find_vma_conflict(mm_ctx, search_addr, size);
        if (conflict) {
            // Move past this VMA and align to page boundary
            search_addr = PAGE_ALIGN_UP(conflict->end);
        } else {
            // This shouldn't happen, but just in case
            search_addr += PAGE_SIZE;
        }
    }
    
    return 0;  // No free range found
}

__PRIVILEGED_CODE
uintptr_t find_free_vma_range(mm_context* mm_ctx, size_t size, uint64_t flags, uintptr_t preferred_addr) {
    __unused flags;
    
    if (!mm_ctx || size == 0) {
        return 0;
    }
    
    // Handle preferred address (used for MAP_FIXED and hints)
    if (preferred_addr) {
        // Validate that preferred address is in user space
        if (preferred_addr < USERSPACE_START || 
            preferred_addr > USERSPACE_END - size) {
            return 0;
        }
        
        // Check if the preferred range is free
        if (!find_vma_conflict(mm_ctx, preferred_addr, size)) {
            return preferred_addr;
        }
        
        // For MAP_FIXED, we must use the exact address (caller handles unmapping)
        // For hints, we fall through to automatic allocation
        return 0;
    }
    
    // Automatic allocation in mmap region
    return find_free_mmap_range(mm_ctx, size);
}

__PRIVILEGED_CODE
vma_area* create_vma(mm_context* mm_ctx, uintptr_t start, size_t size, uint64_t flags, uint64_t type,
                     void* file_backing, uint64_t file_offset) {
    if (!mm_ctx) {
        return nullptr;
    }

    // Validate address range
    if (start < USERSPACE_START || start > USERSPACE_END - size) {
        return nullptr;
    }

    // Create new VMA
    vma_area* new_vma = new vma_area();
    if (!new_vma) {
        return nullptr;
    }

    new_vma->start = start;
    new_vma->end = start + size;
    new_vma->flags = flags;
    new_vma->type = type;
    new_vma->file_backing = file_backing;
    new_vma->file_offset = file_offset;
    new_vma->next = nullptr;
    new_vma->prev = nullptr;

    // Insert into VMA list
    if (!mm_ctx->vma_list) {
        // First VMA
        mm_ctx->vma_list = new_vma;
    } else {
        vma_area* current = mm_ctx->vma_list;
        vma_area* prev = nullptr;

        // Find insertion point (keep list sorted by start address)
        while (current && current->start < start) {
            prev = current;
            current = current->next;
        }

        // Insert new_vma between prev and current
        new_vma->next = current;
        new_vma->prev = prev;

        if (prev) {
            prev->next = new_vma;
        } else {
            mm_ctx->vma_list = new_vma;
        }

        if (current) {
            current->prev = new_vma;
        }
    }

    mm_ctx->vma_count++;
    return new_vma;
}

__PRIVILEGED_CODE
bool remove_vma(mm_context* mm_ctx, vma_area* vma) {
    if (!mm_ctx || !vma) {
        return false;
    }

    // Update list pointers
    if (vma->prev) {
        vma->prev->next = vma->next;
    } else {
        mm_ctx->vma_list = vma->next;
    }

    if (vma->next) {
        vma->next->prev = vma->prev;
    }

    // Free the VMA
    delete vma;
    mm_ctx->vma_count--;

    return true;
}

__PRIVILEGED_CODE
vma_area* find_vma(mm_context* mm_ctx, uintptr_t addr) {
    if (!mm_ctx) {
        return nullptr;
    }

    vma_area* vma = mm_ctx->vma_list;
    while (vma) {
        if (addr >= vma->start && addr < vma->end) {
            return vma;
        }
        vma = vma->next;
    }

    return nullptr;
}

__PRIVILEGED_CODE
bool check_vma_flags(mm_context* mm_ctx, uintptr_t addr, uint64_t flags) {
    vma_area* vma = find_vma(mm_ctx, addr);
    if (!vma) {
        return false;
    }

    return (vma->flags & flags) == flags;
}

__PRIVILEGED_CODE
bool merge_vmas(mm_context* mm_ctx, vma_area* vma) {
    if (!mm_ctx || !vma) {
        return false;
    }

    bool merged = false;

    // Try to merge with next VMA
    if (vma->next && vma->end == vma->next->start &&
        vma->flags == vma->next->flags &&
        vma->type == vma->next->type &&
        vma->file_backing == vma->next->file_backing) {
        
        vma_area* next_vma = vma->next;
        
        // Extend current VMA to include next VMA
        vma->end = next_vma->end;
        
        // Remove next VMA from list
        vma->next = next_vma->next;
        if (next_vma->next) {
            next_vma->next->prev = vma;
        }
        
        delete next_vma;
        mm_ctx->vma_count--;
        merged = true;
    }

    // Try to merge with previous VMA
    if (vma->prev && vma->prev->end == vma->start &&
        vma->prev->flags == vma->flags &&
        vma->prev->type == vma->type &&
        vma->prev->file_backing == vma->file_backing) {
        
        vma_area* prev_vma = vma->prev;
        
        // Extend previous VMA to include current VMA
        prev_vma->end = vma->end;
        
        // Remove current VMA from list
        prev_vma->next = vma->next;
        if (vma->next) {
            vma->next->prev = prev_vma;
        }
        
        delete vma;
        mm_ctx->vma_count--;
        merged = true;
    }

    return merged;
}

__PRIVILEGED_CODE
vma_area* split_vma(mm_context* mm_ctx, vma_area* vma, uintptr_t split_addr) {
    if (!mm_ctx || !vma || split_addr <= vma->start || split_addr >= vma->end) {
        return nullptr;
    }

    // Create new VMA for the second half
    vma_area* new_vma = create_vma(
        mm_ctx,
        split_addr,
        vma->end - split_addr,
        vma->flags,
        vma->type,
        vma->file_backing,
        vma->file_offset + (split_addr - vma->start)
    );

    if (!new_vma) {
        return nullptr;
    }

    // Update original VMA to cover only the first half
    vma->end = split_addr;

    return new_vma;
}

__PRIVILEGED_CODE
void dbg_print_vma_regions(const mm_context* mm_ctx, const char* process_name) {
    if (!mm_ctx) {
        kprint("[VMA] Invalid memory context\n");
        return;
    }

    kprint("\n[VMA] Memory map for process: %s\n", process_name ? process_name : "unnamed");
    kprint("+---------------------------------------------------------------------------------+\n");
    kprint("|        Start         |         End          |   Size   |   Prot   |    Type    |\n");
    kprint("+---------------------------------------------------------------------------------+\n");

    vma_area* vma = mm_ctx->vma_list;
    while (vma) {
        // Calculate size in human-readable format
        uint64_t size = vma->end - vma->start;
        const char* size_unit = "B";
        if (size >= 1024 * 1024 * 1024) {
            size = size / (1024 * 1024 * 1024);
            size_unit = "GB";
        } else if (size >= 1024 * 1024) {
            size = size / (1024 * 1024);
            size_unit = "MB";
        } else if (size >= 1024) {
            size = size / 1024;
            size_unit = "KB";
        }

        // Build protection string
        char prot[5] = "----";
        if (vma->flags & VMA_PROT_READ) prot[0] = 'r';
        if (vma->flags & VMA_PROT_WRITE) prot[1] = 'w';
        if (vma->flags & VMA_PROT_EXEC) prot[2] = 'x';
        prot[3] = 'p';  // Present

        // Build type string
        const char* type = "unknown";
        if (vma->type & VMA_TYPE_ANONYMOUS) {
            if (vma->type & VMA_TYPE_PRIVATE) type = "anon-priv";
            else if (vma->type & VMA_TYPE_SHARED) type = "anon-shr";
            else type = "anonymous";
        } else if (vma->type & VMA_TYPE_FILE) {
            if (vma->type & VMA_TYPE_PRIVATE) type = "file-priv";
            else if (vma->type & VMA_TYPE_SHARED) type = "file-shr";
            else type = "file";
        }

        kprint("| %016llx     | %016llx     | %llu%s    | %s    | %s    |\n",
            vma->start, vma->end, size, size_unit, prot, type);

        vma = vma->next;
    }

    kprint("+---------------------------------------------------------------------------------+\n");
    kprint("Total VMAs: %llu, mmap_base: 0x%llx\n\n", mm_ctx->vma_count, mm_ctx->mmap_base);
}
