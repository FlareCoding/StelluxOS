#include "gpu/core.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/shmem.h"
#include "mm/vma.h"
#include "resource/providers/shmem_provider.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/mutex.h"

namespace gpu {

constexpr uint32_t KNOWN_BUFFER_FLAGS = GPU_BUFFER_MAPPABLE | GPU_BUFFER_SCANOUT;

namespace {

// A shared memory object's pages as an import read them, with the pin that keeps them allocated
struct pinned_pages {
    mm::shmem_mapping pin;
    pmm::phys_addr_t* pages = nullptr;
    size_t            count = 0;
};

} // namespace

__PRIVILEGED_CODE void buffer::release_memory() {
    m_device.get_backend().release_memory(m_memory);
    heap::ufree_delete(this);
}

__PRIVILEGED_CODE void drop_buffer(buffer* buf) {
    if (buf->release()) {
        mm::device_backing::ref_destroy(buf);
    }
}

__PRIVILEGED_CODE static void buffer_close(resource::resource_object* obj) {
    drop_buffer(static_cast<buffer*>(obj->impl));
    obj->impl = nullptr;
}

__PRIVILEGED_CODE static int32_t buffer_mmap(
    resource::resource_object* obj,
    mm::mm_context* mm_ctx,
    uintptr_t addr,
    size_t length,
    uint32_t prot,
    uint32_t max_prot,
    uint32_t map_flags,
    uint64_t offset,
    uintptr_t* out_addr
) {
    auto* buf = static_cast<buffer*>(obj->impl);
    size_t aligned_len = pmm::page_align_up(length);

    bool mappable = (buf->flags() & GPU_BUFFER_MAPPABLE) != 0;
    bool fits = aligned_len >= length && aligned_len <= buf->size() && offset <= buf->size() - aligned_len;
    if (!mappable || !fits) {
        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    cpu_view view = {};
    int32_t rc = buf->owner().get_backend().view_for_cpu(buf->backend_memory(), view);
    bool aligned = mm::is_page_aligned(view.phys);
    bool covers = view.size >= offset + aligned_len;
    if (rc != OK || !aligned || !covers) {
        return mm::MM_CTX_ERR_MAP_FAILED;
    }

    uint32_t cache = view.cacheable ? paging::PAGE_NORMAL : paging::PAGE_WC;

    return mm::mm_context_map_device(
        mm_ctx,
        view.phys + offset,
        length,
        buf,
        offset,
        prot,
        max_prot,
        cache,
        map_flags,
        addr,
        out_addr
    );
}

static const resource::resource_ops g_buffer_ops = {
    .close = buffer_close,
    .mmap = buffer_mmap,
};

static bool valid_create(const device& dev, const gpu_buffer_args& args) {
    bool known_placement = args.placement >= GPU_PLACEMENT_VIDEO && args.placement <= GPU_PLACEMENT_CONTEXT;
    bool offered = known_placement && (dev.info().placements & (1u << args.placement)) != 0;
    bool known_flags = (args.flags & ~KNOWN_BUFFER_FLAGS) == 0;
    bool scanout_outside_video = (args.flags & GPU_BUFFER_SCANOUT) != 0 && args.placement != GPU_PLACEMENT_VIDEO;
    bool by_context = args.placement == GPU_PLACEMENT_CONTEXT;
    bool context_clear = by_context || (args.context == 0 && args.backend_id == 0);

    return args.size != 0 && offered && known_flags && !scanout_outside_video && args.source == 0 && context_clear;
}

// The resource object of a new buffer the backend has yet to give memory, or null without kernel memory
__PRIVILEGED_CODE static resource::resource_object* new_object(
    device& dev,
    uint64_t size,
    uint32_t placement,
    uint32_t flags
) {
    auto* buf = heap::ualloc_new<buffer>(dev, size, placement, flags);
    if (!buf) {
        return nullptr;
    }

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::ufree_delete(buf);
        return nullptr;
    }

    obj->type = resource::resource_type::GPU_BUFFER;
    obj->ops = &g_buffer_ops;
    obj->impl = buf;

    return obj;
}

// Frees a new object that no handle reached and the backend gave no memory
__PRIVILEGED_CODE static void free_object(resource::resource_object* obj) {
    heap::ufree_delete(static_cast<buffer*>(obj->impl));
    heap::kfree_delete(obj);
}

__PRIVILEGED_CODE static int32_t new_buffer(
    device& dev,
    const gpu_buffer_args& args,
    uint64_t size,
    space* s,
    uint64_t arg
) {
    resource::resource_object* obj = new_object(dev, size, args.placement, args.flags);
    if (!obj) {
        return fs::ERR_NOMEM;
    }

    memory* mem = nullptr;
    uint32_t device_id = 0;
    int32_t rc = dev.get_backend().create_memory(
        size,
        args.placement,
        args.flags,
        s,
        args.backend_id,
        &mem,
        &device_id
    );
    if (rc != OK) {
        free_object(obj);
        return to_fs_error(rc);
    }

    static_cast<buffer*>(obj->impl)->take(mem, device_id);
    rc = copy_out(arg + __builtin_offsetof(gpu_buffer_args, device_id), &device_id, sizeof(device_id));
    if (rc == fs::OK) {
        uint64_t user_handle = arg + __builtin_offsetof(gpu_buffer_args, buffer);
        rc = install_handle(obj, resource::RIGHT_READ | resource::RIGHT_WRITE, user_handle);
    }

    resource::resource_release(obj);

    return rc;
}

__PRIVILEGED_CODE int32_t create_buffer(device& dev, uint64_t arg) {
    gpu_buffer_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    uint64_t size = pmm::page_align_up(args.size);
    if (!valid_create(dev, args) || size < args.size) {
        return fs::ERR_INVAL;
    }

    if (dev.failed()) {
        return fs::ERR_IO;
    }

    if (args.placement != GPU_PLACEMENT_CONTEXT) {
        return new_buffer(dev, args, size, nullptr, arg);
    }

    // The call's context reference keeps the space until the backend returns, and the buffer holds none
    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_on_device(dev, args.context, resource::resource_type::GPU_CONTEXT, resource::RIGHT_WRITE, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    auto& c = *static_cast<context*>(obj->impl);
    uint64_t fault_va = 0;
    rc = lost_state(c, &fault_va) != 0 ? fs::ERR_IO : new_buffer(dev, args, size, c.backend_space, arg);
    resource::resource_release(obj);

    return rc;
}

// The report is written before the handle is installed, so a report the kernel cannot write leaves no handle
__PRIVILEGED_CODE static int32_t report_import(
    resource::resource_object* obj,
    uint32_t rights,
    gpu_buffer_args& args,
    uint64_t arg
) {
    const auto& buf = *static_cast<const buffer*>(obj->impl);
    args.size = buf.size();
    args.placement = buf.placement();
    args.flags = buf.flags();
    args.device_id = buf.device_id();

    int32_t rc = copy_out(arg, &args, sizeof(args));
    if (rc != fs::OK) {
        return rc;
    }

    return install_handle(obj, rights, arg + __builtin_offsetof(gpu_buffer_args, buffer));
}

// Starts a pin and reads the pages under the object's lock, so no shrink frees a page first
__PRIVILEGED_CODE static int32_t pin_pages(mm::shmem& shared, pinned_pages& out) {
    sync::mutex_lock(shared.lock);
    out.count = shared.m_page_count;
    if (out.count != 0) {
        out.pages = static_cast<pmm::phys_addr_t*>(allocate(out.count * sizeof(pmm::phys_addr_t)));
    }

    if (out.pages) {
        out.pin = mm::shmem_mapping::start_locked(&shared);
        for (size_t i = 0; i < out.count; i++) {
            out.pages[i] = mm::shmem_get_page_locked(&shared, i);
        }
    }

    sync::mutex_unlock(shared.lock);

    if (out.count == 0) {
        return fs::ERR_INVAL;
    }

    return out.pages ? fs::OK : fs::ERR_NOMEM;
}

// A wrap failing with ERR_DEVICE took the pin, and on any other failure the pin stays in `pinned`, released with it
__PRIVILEGED_CODE static int32_t wrap_shared_memory(
    device& dev,
    pinned_pages& pinned,
    resource::resource_object** out
) {
    resource::resource_object* obj = new_object(dev, pinned.count * pmm::PAGE_SIZE, GPU_PLACEMENT_SYSTEM, 0);
    if (!obj) {
        return fs::ERR_NOMEM;
    }

    memory* mem = nullptr;
    uint32_t device_id = 0;
    int32_t rc = dev.get_backend().wrap_pages(
        pinned.pages,
        pinned.count,
        static_cast<mm::shmem_mapping&&>(pinned.pin),
        &mem,
        &device_id
    );
    if (rc != OK) {
        free_object(obj);
        return to_fs_error(rc);
    }

    static_cast<buffer*>(obj->impl)->take(mem, device_id);
    *out = obj;

    return fs::OK;
}

__PRIVILEGED_CODE static int32_t import_shared_memory(
    device& dev,
    mm::shmem& shared,
    uint32_t rights,
    gpu_buffer_args& args,
    uint64_t arg
) {
    if ((dev.info().placements & (1u << GPU_PLACEMENT_SYSTEM)) == 0) {
        return fs::ERR_INVAL;
    }

    if (dev.failed()) {
        return fs::ERR_IO;
    }

    pinned_pages pinned = {};
    resource::resource_object* obj = nullptr;
    int32_t rc = pin_pages(shared, pinned);
    if (rc == fs::OK) {
        rc = wrap_shared_memory(dev, pinned, &obj);
    }

    if (pinned.pages) {
        heap::ufree(pinned.pages);
    }

    if (rc != fs::OK) {
        return rc;
    }

    rc = report_import(obj, rights, args, arg);
    resource::resource_release(obj);

    return rc;
}

__PRIVILEGED_CODE int32_t import_buffer(device& dev, uint64_t arg) {
    gpu_buffer_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    if (args.context != 0 || args.backend_id != 0) {
        return fs::ERR_INVAL;
    }

    resource::resource_object* source = nullptr;
    uint32_t rights = 0;
    int32_t found = resource::get_handle_object(
        sched::current()->handles,
        args.source,
        resource::RIGHT_READ,
        &source,
        nullptr,
        &rights
    );
    if (found != resource::HANDLE_OK) {
        return fs::ERR_BADF;
    }

    mm::shmem* shared = resource::shmem_provider::get_shmem_backing(source);
    bool is_buffer = source->type == resource::resource_type::GPU_BUFFER;
    int32_t rc = fs::ERR_BADF;
    if (is_buffer && &static_cast<buffer*>(source->impl)->owner() != &dev) {
        rc = fs::ERR_INVAL;
    } else if (is_buffer) {
        rc = dev.failed() ? fs::ERR_IO : report_import(source, rights, args, arg);
    } else if (shared) {
        rc = import_shared_memory(dev, *shared, rights, args, arg);
    }

    resource::resource_release(source);

    return rc;
}

__PRIVILEGED_CODE int32_t lookup_buffer(int32_t handle, uint32_t rights, buffer** out, memory_view& view) {
    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_object(handle, resource::resource_type::GPU_BUFFER, rights, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    auto* buf = static_cast<buffer*>(obj->impl);
    buf->add_ref();
    resource::resource_release(obj);

    *out = buf;
    view = {
        .owner     = &buf->owner().get_backend(),
        .mem       = buf->backend_memory(),
        .size      = buf->size(),
        .placement = buf->placement(),
        .flags     = buf->flags(),
    };

    return fs::OK;
}

__PRIVILEGED_CODE void release_buffer(buffer* buf) {
    drop_buffer(buf);
}

} // namespace gpu
