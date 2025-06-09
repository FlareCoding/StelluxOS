#include <process/vma.h>
#include <memory/paging.h>
#include <memory/vmm.h>
#include <core/klog.h>

// Constants for VMA management
constexpr uintptr_t USERSPACE_START = 0x0;
constexpr uintptr_t USERSPACE_END = 0x7fffffffffff;
constexpr size_t DEFAULT_MMAP_GAP = 0x1000000; // 16MB gap

__PRIVILEGED_CODE
bool init_process_vma(mm_context* mm_ctx) {
    if (!mm_ctx) {
        return false;
    }

    // Initialize VMA management
    mm_ctx->vma_list = nullptr;
    mm_ctx->mmap_base = USERSPACE_END - DEFAULT_MMAP_GAP;
    mm_ctx->task_size = USERSPACE_END - USERSPACE_START;
    mm_ctx->vma_count = 0;

    return true;
}

__PRIVILEGED_CODE
uintptr_t find_free_vma_range(mm_context* mm_ctx, size_t size, uint64_t flags, uintptr_t preferred_addr) {
    __unused flags;
    
    if (!mm_ctx) {
        return 0;
    }

    // If preferred address is specified, check if it's available
    if (preferred_addr) {
        if (preferred_addr < mm_ctx->mmap_base || 
            preferred_addr + size > USERSPACE_END) {
            return 0;
        }

        // Check if the range overlaps with any existing VMA
        vma_area* vma = mm_ctx->vma_list;
        while (vma) {
            if ((preferred_addr >= vma->start && preferred_addr < vma->end) ||
                (preferred_addr + size > vma->start && preferred_addr + size <= vma->end)) {
                return 0;
            }
            vma = vma->next;
        }

        return preferred_addr;
    }

    // First, try to find a gap between existing VMAs within the mmap-able region
    vma_area* vma = mm_ctx->vma_list;
    while (vma && vma->next) {
        // Only consider gaps that are within the mmap-able region
        if (vma->end >= mm_ctx->mmap_base && vma->next->start <= USERSPACE_END) {
            uintptr_t gap_start = vma->end;
            uintptr_t gap_end = vma->next->start;
            size_t gap_size = gap_end - gap_start;

            if (gap_size >= size) {
                return gap_start;  // Found a suitable gap
            }
        }
        vma = vma->next;
    }

    // If no suitable gap found, search from top down starting from mmap_base
    uintptr_t current_addr = mm_ctx->mmap_base;
    bool found_gap = false;

    while (current_addr + size <= USERSPACE_END) {
        found_gap = true;
        vma = mm_ctx->vma_list;
        uintptr_t gap_end = USERSPACE_END;

        // Find the end of the current gap
        while (vma) {
            if (vma->start > current_addr && vma->start < gap_end) {
                gap_end = vma->start;
            }
            if ((current_addr >= vma->start && current_addr < vma->end) ||
                (current_addr + size > vma->start && current_addr + size <= vma->end)) {
                found_gap = false;
                // Move to the end of this VMA
                current_addr = vma->end;
                break;
            }
            vma = vma->next;
        }

        if (found_gap) {
            // For top-down allocation, we want to place the new region at the end of the gap
            // minus the size we want to allocate
            return gap_end - size;
        }
    }

    return 0;
}

__PRIVILEGED_CODE
vma_area* create_vma(mm_context* mm_ctx, uintptr_t start, size_t size, uint64_t flags, uint64_t type,
                     void* file_backing, uint64_t file_offset) {
    if (!mm_ctx) {
        return nullptr;
    }

    // Validate address range
    if (start < USERSPACE_START || start + size > USERSPACE_END) {
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
        mm_ctx->vma_list = new_vma;
    } else {
        vma_area* current = mm_ctx->vma_list;
        vma_area* prev = nullptr;

        while (current && current->start < start) {
            prev = current;
            current = current->next;
        }

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

    // Try to merge with next VMA
    if (vma->next && vma->end == vma->next->start &&
        vma->flags == vma->next->flags &&
        vma->type == vma->next->type) {
        
        vma->end = vma->next->end;
        remove_vma(mm_ctx, vma->next);
        return true;
    }

    // Try to merge with previous VMA
    if (vma->prev && vma->prev->end == vma->start &&
        vma->prev->flags == vma->flags &&
        vma->prev->type == vma->type) {
        
        vma->prev->end = vma->end;
        remove_vma(mm_ctx, vma);
        return true;
    }

    return false;
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

    // Update original VMA
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
    kprint("+-----------------------------------------------------------------------------+\n");
    kprint("|        Start         |         End          |   Size   |   Prot   |  Type   |\n");
    kprint("+-----------------------------------------------------------------------------+\n");

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
        if (vma->type & VMA_TYPE_PRIVATE) type = "private";
        else if (vma->type & VMA_TYPE_SHARED) type = "shared";
        else if (vma->type & VMA_TYPE_ANONYMOUS) type = "anon";
        else if (vma->type & VMA_TYPE_FILE) type = "file";

        kprint("| %016llx     | %016llx     | %llu%s    | %s    | %s    |\n",
            vma->start, vma->end, size, size_unit, prot, type);

        vma = vma->next;
    }

    kprint("+-----------------------------------------------------------------------------+\n");
    kprint("Total VMAs: %llu\n\n", mm_ctx->vma_count);
}
