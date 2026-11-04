#include "gpu/core.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "resource/resource.h"

namespace gpu {

constexpr uint32_t KNOWN_BUFFER_FLAGS = GPU_BUFFER_MAPPABLE | GPU_BUFFER_SCANOUT;

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

__PRIVILEGED_CODE static int32_t new_buffer(
    device& dev,
    const gpu_buffer_args& args,
    uint64_t size,
    space* s,
    uint64_t arg
) {
    auto* buf = heap::ualloc_new<buffer>(dev, size, args.flags);
    if (!buf) {
        return fs::ERR_NOMEM;
    }

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::ufree_delete(buf);
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
        heap::kfree_delete(obj);
        heap::ufree_delete(buf);
        return to_fs_error(rc);
    }

    buf->take(mem, device_id);
    obj->type = resource::resource_type::GPU_BUFFER;
    obj->ops = &g_buffer_ops;
    obj->impl = buf;

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

    if (args.placement != GPU_PLACEMENT_CONTEXT) {
        return new_buffer(dev, args, size, nullptr, arg);
    }

    // The call's context reference keeps the space until the backend returns, and the buffer holds none
    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_on_device(dev, args.context, resource::resource_type::GPU_CONTEXT, resource::RIGHT_WRITE, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    rc = new_buffer(dev, args, size, static_cast<context*>(obj->impl)->backend_space, arg);
    resource::resource_release(obj);

    return rc;
}

} // namespace gpu
