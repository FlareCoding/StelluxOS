#include <ipc/shm.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <memory/allocators/page_bitmap_allocator.h>
#include <dynpriv/dynpriv.h>
#include <process/process.h>
#include <process/vma.h>
#include <process/mm.h>
#include <core/klog.h>

namespace ipc {

shm_handle_t shared_memory::s_next_id = 0;
mutex shared_memory::s_global_lock = mutex();
kstl::hashmap<shm_handle_t, kstl::shared_ptr<shm_region>> shared_memory::s_regions;
kstl::hashmap<kstl::string, shm_handle_t> shared_memory::s_name_map;

static size_t _pages_for_size(size_t size) {
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

shm_region* shared_memory::get_region(shm_handle_t handle) {
    mutex_guard guard(s_global_lock);
    if (!s_regions.find(handle)) {
        return nullptr;
    }
    return s_regions[handle].get();
}

shm_handle_t shared_memory::create(const kstl::string& name, size_t size, shm_access policy) {
    mutex_guard guard(s_global_lock);

    // Initialize hashmaps at runtime
    if (s_next_id == 0) {
       s_regions = kstl::hashmap<shm_handle_t, kstl::shared_ptr<shm_region>>();
       s_name_map = kstl::hashmap<kstl::string, shm_handle_t>();
       s_next_id++;
    }

    if (s_name_map.find(name)) {
        return 0; // Name already exists
    }

    shm_handle_t id = s_next_id++;
    kstl::shared_ptr<shm_region> region = kstl::make_shared<shm_region>();
    region->id = id;
    region->name = name;
    region->size = size;
    region->policy = policy;
    region->ref_count = 0;
    region->pending_delete = false;

    size_t page_count = _pages_for_size(size);
    for (size_t i = 0; i < page_count; ++i) {
        void* phys = nullptr;
        RUN_ELEVATED({
            phys = allocators::page_bitmap_allocator::get_physical_allocator().alloc_page();
        });
        if (!phys) {
            // On failure free previously allocated pages
            RUN_ELEVATED({
                for (uintptr_t p : region->pages) {
                    allocators::page_bitmap_allocator::get_physical_allocator().free_page(reinterpret_cast<void*>(p));
                }
            });
            return 0;
        }
        region->pages.push_back(reinterpret_cast<uintptr_t>(phys));
    }

    s_regions[id] = region;
    s_name_map[name] = id;
    return id;
}

shm_handle_t shared_memory::open(const kstl::string& name) {
    mutex_guard guard(s_global_lock);
    if (!s_name_map.find(name)) {
        return 0;
    }
    return s_name_map[name];
}

bool shared_memory::destroy(shm_handle_t handle) {
    shm_region* region = get_region(handle);
    if (!region) {
        return false;
    }

    mutex_guard region_guard(region->lock);
    region->pending_delete = true;
    if (region->ref_count == 0) {
        RUN_ELEVATED({
            for (uintptr_t p : region->pages) {
                allocators::page_bitmap_allocator::get_physical_allocator().free_page(reinterpret_cast<void*>(p));
            }
        });
        {
            mutex_guard guard(s_global_lock);
            s_name_map.remove(region->name);
            s_regions.remove(handle);
        }
    }
    return true;
}

void* shared_memory::map(shm_handle_t handle, uint64_t flags, shm_mapping_context context) {
    shm_region* region = get_region(handle);
    if (!region) {
        return nullptr;
    }

    mutex_guard region_guard(region->lock);
    const size_t page_count = region->pages.size();

    if (context == shm_mapping_context::KERNEL) {
        // Kernel mapping path - use existing implementation
        void* virt_base = nullptr;

        RUN_ELEVATED({
            virt_base = vmm::alloc_contiguous_virtual_pages(page_count, flags);
        });
        
        if (!virt_base) {
            return nullptr;
        }

        RUN_ELEVATED({
            for (size_t i = 0; i < page_count; ++i) {
                paging::map_page(
                    reinterpret_cast<uintptr_t>(virt_base) + i * PAGE_SIZE,
                    region->pages[i],
                    flags,
                    paging::get_pml4()
                );
            }
        });

        region->ref_count++;
        return virt_base;
    } else {
        // Userland mapping path - similar to mmap implementation
        if (!current || !current->get_core()) {
            return nullptr;
        }

        mm_context* mm_ctx = &current->get_core()->mm_ctx;
        size_t mapping_size = page_count * PAGE_SIZE;

        // Find free virtual address range in userland space
        uintptr_t target_addr = find_free_vma_range(mm_ctx, mapping_size, 0, 0);
        if (!target_addr) {
            return nullptr; // No free address range found
        }

        // Convert shm_access policy to VMA protection flags
        uint64_t vma_prot = 0;
        if (region->policy == shm_access::READ_ONLY || region->policy == shm_access::READ_WRITE) {
            vma_prot |= VMA_PROT_READ;
        }
        if (region->policy == shm_access::READ_WRITE) {
            vma_prot |= VMA_PROT_WRITE;
        }

        // Create VMA for the shared memory region
        uint64_t vma_type = VMA_TYPE_SHARED | VMA_TYPE_ANONYMOUS;
        vma_area* vma = create_vma(mm_ctx, target_addr, mapping_size, vma_prot, vma_type);
        if (!vma) {
            return nullptr; // Failed to create VMA
        }

        // Convert protection flags to page table flags
        uint64_t page_flags = PTE_PRESENT | PTE_US;  // User accessible
        if (region->policy == shm_access::READ_WRITE) {
            page_flags |= PTE_RW;
        }
        // Note: We don't set PTE_NX since shared memory should be non-executable

        // Map the shared memory pages into userland virtual memory
        paging::page_table* page_table = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);
        bool mapping_failed = false;

        RUN_ELEVATED({
            for (size_t i = 0; i < page_count; ++i) {
                uintptr_t virt_addr = target_addr + (i * PAGE_SIZE);
                uintptr_t phys_addr = region->pages[i];

                paging::map_page(virt_addr, phys_addr, page_flags, page_table);
            }
        });

        if (mapping_failed) {
            // Clean up: unmap any successfully mapped pages and remove VMA
            RUN_ELEVATED({
                for (size_t i = 0; i < page_count; ++i) {
                    uintptr_t virt_addr = target_addr + (i * PAGE_SIZE);
                    paging::unmap_page(virt_addr, page_table);
                }
            });
            remove_vma(mm_ctx, vma);
            return nullptr;
        }

        region->ref_count++;
        return reinterpret_cast<void*>(target_addr);
    }
}

bool shared_memory::unmap(shm_handle_t handle, void* addr, shm_mapping_context context) {
    shm_region* region = get_region(handle);
    if (!region || !addr) {
        return false;
    }

    mutex_guard region_guard(region->lock);
    size_t page_count = region->pages.size();

    if (context == shm_mapping_context::KERNEL) {
        // Kernel unmapping path - use existing implementation
        RUN_ELEVATED({
            vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(addr), page_count);
        });
    } else {
        // Userland unmapping path - similar to munmap implementation
        if (!current || !current->get_core()) {
            return false;
        }

        mm_context* mm_ctx = &current->get_core()->mm_ctx;
        uintptr_t virt_addr = reinterpret_cast<uintptr_t>(addr);
        size_t mapping_size = page_count * PAGE_SIZE;

        // Find the VMA containing this address
        vma_area* vma = find_vma(mm_ctx, virt_addr);
        if (!vma || vma->start != virt_addr || (vma->end - vma->start) != mapping_size) {
            return false; // Invalid address or size mismatch
        }

        // Unmap the pages from userland virtual memory
        paging::page_table* page_table = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);

        RUN_ELEVATED({
            for (size_t i = 0; i < page_count; ++i) {
                uintptr_t page_addr = virt_addr + (i * PAGE_SIZE);
                paging::unmap_page(page_addr, page_table);
            }
        });

        // Remove the VMA
        remove_vma(mm_ctx, vma);
    }

    if (region->ref_count > 0) {
        region->ref_count--;
    }

    if (region->pending_delete && region->ref_count == 0) {
        RUN_ELEVATED({
            for (uintptr_t p : region->pages) {
                allocators::page_bitmap_allocator::get_physical_allocator().free_page(reinterpret_cast<void*>(p));
            }
        });
        {
            mutex_guard guard(s_global_lock);
            s_name_map.remove(region->name);
            s_regions.remove(handle);
        }
    }

    return true;
}

} // namespace ipc
