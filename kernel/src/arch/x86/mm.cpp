#ifdef ARCH_X86_64
#include <process/mm.h>
#include <memory/memory.h>
#include <memory/paging.h>
#include <memory/vmm.h>
#include <process/vma.h>
#include <memory/allocators/page_bitmap_allocator.h>
#include <core/klog.h>

__PRIVILEGED_CODE
mm_context save_mm_context() {
    mm_context ctx;
    zeromem(&ctx, sizeof(mm_context));

    ctx.root_page_table = reinterpret_cast<uint64_t>(paging::get_pml4());

    return ctx;
}

/**
 * @brief Installs a memory management context into the MMU.
 * 
 * Updates the MMU to use the given memory management context. This function
 * typically writes to the CR3 register or its platform-specific equivalent to
 * switch the page tables.
 * 
 * @param context The memory management context to install.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
void install_mm_context(const mm_context& context) {
    paging::set_pml4(reinterpret_cast<paging::page_table*>(context.root_page_table));
}

__PRIVILEGED_CODE
bool manage_process_heap(mm_context* mm_ctx, uintptr_t new_heap_end) {
    if (!mm_ctx) {
        return false;
    }

    // Page-align the new heap end
    new_heap_end = PAGE_ALIGN_UP(new_heap_end);

    // If this is the first heap allocation and heap_start is 0, initialize it
    if (mm_ctx->heap_start == 0) {
        mm_ctx->heap_start = new_heap_end;
        mm_ctx->heap_end = new_heap_end;
        return true;
    }

    // Cannot shrink below heap start
    if (new_heap_end < mm_ctx->heap_start) {
        return false;
    }

    uintptr_t current_heap_end = PAGE_ALIGN_UP(mm_ctx->heap_end);
    
    if (new_heap_end == current_heap_end) {
        // No change needed
        mm_ctx->heap_end = new_heap_end;
        return true;
    }

    paging::page_table* pt = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);
    auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();

    if (new_heap_end > current_heap_end) {
        // Growing the heap
        size_t num_pages = (new_heap_end - current_heap_end) / PAGE_SIZE;
        
        // Allocate and map individual pages (no need for physical contiguity)
        for (size_t i = 0; i < num_pages; i++) {
            uintptr_t virt_addr = current_heap_end + (i * PAGE_SIZE);
            
            // Allocate a single physical page
            void* phys_page = physalloc.alloc_page();
            if (!phys_page) {
                // Cleanup on failure: unmap and free any pages we've already allocated
                for (size_t j = 0; j < i; j++) {
                    uintptr_t cleanup_virt = current_heap_end + (j * PAGE_SIZE);
                    void* cleanup_phys = reinterpret_cast<void*>(
                        paging::get_physical_address(reinterpret_cast<void*>(cleanup_virt), pt)
                    );
                    if (cleanup_phys) {
                        paging::unmap_page(cleanup_virt, pt);
                        physalloc.free_page(cleanup_phys);
                    }
                }
                return false;
            }

            // Map the single page
            paging::map_page(
                virt_addr,
                reinterpret_cast<uintptr_t>(phys_page),
                DEFAULT_UNPRIV_PAGE_FLAGS,
                pt
            );
        }

        // Find existing heap VMA or create new one
        vma_area* heap_vma = find_vma(mm_ctx, mm_ctx->heap_start);
        if (heap_vma && heap_vma->end == current_heap_end) {
            // Extend existing heap VMA
            heap_vma->end = new_heap_end;
        } else {
            // Create new VMA for the heap extension
            vma_area* new_vma = create_vma(
                mm_ctx,
                current_heap_end,
                new_heap_end - current_heap_end,
                VMA_PROT_READ | VMA_PROT_WRITE,
                VMA_TYPE_PRIVATE | VMA_TYPE_ANONYMOUS
            );
            if (!new_vma) {
                // Cleanup on failure: unmap and free individual pages
                for (size_t i = 0; i < num_pages; i++) {
                    uintptr_t page_addr = current_heap_end + (i * PAGE_SIZE);
                    void* phys_addr = reinterpret_cast<void*>(
                        paging::get_physical_address(reinterpret_cast<void*>(page_addr), pt)
                    );
                    if (phys_addr) {
                        paging::unmap_page(page_addr, pt);
                        physalloc.free_page(phys_addr);
                    }
                }
                return false;
            }
        }
    } else {
        // Shrinking the heap
        size_t num_pages = (current_heap_end - new_heap_end) / PAGE_SIZE;
        
        // Unmap and free the pages
        for (size_t i = 0; i < num_pages; i++) {
            uintptr_t page_addr = new_heap_end + (i * PAGE_SIZE);
            void* phys_addr = reinterpret_cast<void*>(paging::get_physical_address(reinterpret_cast<void*>(page_addr), pt));
            
            if (phys_addr) {
                paging::unmap_page(page_addr, pt);
                physalloc.free_page(phys_addr);
            }
        }

        // Update or remove heap VMA
        vma_area* heap_vma = find_vma(mm_ctx, new_heap_end);
        if (heap_vma) {
            if (heap_vma->start >= new_heap_end) {
                // Remove entire VMA if it's completely beyond the new heap end
                remove_vma(mm_ctx, heap_vma);
            } else if (heap_vma->end > new_heap_end) {
                // Shrink the VMA to the new heap end
                heap_vma->end = new_heap_end;
            }
        }
    }

    // Update heap end
    mm_ctx->heap_end = new_heap_end;
    return true;
}
#endif // ARCH_X86_64

