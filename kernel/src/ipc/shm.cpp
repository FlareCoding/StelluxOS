#include <ipc/shm.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <dynpriv/dynpriv.h>

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

void* shared_memory::map(shm_handle_t handle, uint64_t flags) {
    shm_region* region = get_region(handle);
    if (!region) {
        return nullptr;
    }

    mutex_guard region_guard(region->lock);

    const size_t page_count = region->pages.size();
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
}

bool shared_memory::unmap(shm_handle_t handle, void* addr) {
    shm_region* region = get_region(handle);
    if (!region || !addr) {
        return false;
    }

    mutex_guard region_guard(region->lock);
    size_t page_count = region->pages.size();

    RUN_ELEVATED({
        vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(addr), page_count);
    });

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
