#include "resource/providers/file_provider.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "mm/heap.h"
#include "mm/vma.h"

namespace resource::file_provider {

struct file_resource_impl {
    fs::file* file;
};

__PRIVILEGED_CODE static int32_t map_fs_error_to_resource(int32_t fs_err) {
    switch (fs_err) {
        case fs::ERR_NOENT:
            return ERR_NOENT;
        case fs::ERR_NOMEM:
            return ERR_NOMEM;
        case fs::ERR_NOTDIR:
            return ERR_NOTDIR;
        case fs::ERR_NAMETOOLONG:
            return ERR_NAMETOOLONG;
        case fs::ERR_INVAL:
            return ERR_INVAL;
        case fs::ERR_BADF:
            return ERR_BADF;
        case fs::ERR_NOSYS:
            return ERR_UNSUP;
        case fs::ERR_AGAIN:
            return ERR_AGAIN;
        default:
            return ERR_IO;
    }
}

__PRIVILEGED_CODE static ssize_t file_read(resource_object* obj, void* kdst, size_t count, uint32_t flags) {
    (void)flags;
    if (!obj || !obj->impl || !kdst) {
        return ERR_INVAL;
    }
    auto* impl = static_cast<file_resource_impl*>(obj->impl);
    ssize_t rc = fs::read(impl->file, kdst, count);
    if (rc < 0) {
        return map_fs_error_to_resource(static_cast<int32_t>(rc));
    }
    return rc;
}

__PRIVILEGED_CODE static ssize_t file_write(resource_object* obj, const void* ksrc, size_t count, uint32_t flags) {
    (void)flags;
    if (!obj || !obj->impl || !ksrc) {
        return ERR_INVAL;
    }
    auto* impl = static_cast<file_resource_impl*>(obj->impl);
    ssize_t rc = fs::write(impl->file, ksrc, count);
    if (rc < 0) {
        return map_fs_error_to_resource(static_cast<int32_t>(rc));
    }
    return rc;
}

__PRIVILEGED_CODE static void file_close(resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    auto* impl = static_cast<file_resource_impl*>(obj->impl);
    if (impl->file) {
        fs::close(impl->file);
        impl->file = nullptr;
    }
    heap::kfree_delete(impl);
    obj->impl = nullptr;
}

__PRIVILEGED_CODE static int32_t file_ioctl(resource_object* obj, uint32_t cmd, uint64_t arg) {
    if (!obj || !obj->impl) {
        return ERR_INVAL;
    }
    auto* impl = static_cast<file_resource_impl*>(obj->impl);
    int32_t rc = fs::ioctl(impl->file, cmd, arg);
    if (rc < 0) {
        return map_fs_error_to_resource(rc);
    }
    return rc;
}

__PRIVILEGED_CODE static int32_t file_mmap(
    resource_object* obj, mm::mm_context* mm_ctx,
    uintptr_t addr, size_t length, uint32_t prot,
    uint32_t map_flags, uint64_t offset, uintptr_t* out_addr
) {
    if (!obj || !obj->impl) {
        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    auto* impl = static_cast<file_resource_impl*>(obj->impl);
    int32_t rc = fs::mmap(impl->file, mm_ctx, addr, length, prot, map_flags, offset, out_addr);
    if (rc == 0) {
        return mm::MM_CTX_OK;
    }
    switch (rc) {
    case mm::MM_CTX_ERR_INVALID_ARG:
    case mm::MM_CTX_ERR_NO_MEM:
    case mm::MM_CTX_ERR_NO_VIRT:
    case mm::MM_CTX_ERR_EXISTS:
    case mm::MM_CTX_ERR_MAP_FAILED:
    case mm::MM_CTX_ERR_NOT_MAPPED:
        return rc;
    default:
        return mm::MM_CTX_ERR_INVALID_ARG;
    }
}

static const resource_ops g_file_ops = {
    file_read,
    file_write,
    file_close,
    file_ioctl,
    file_mmap,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t open_file_resource(
    const char* path,
    uint32_t flags,
    resource_object** out_obj
) {
    if (!path || !out_obj) {
        return ERR_INVAL;
    }

    int32_t fs_err = fs::OK;
    fs::file* file = fs::open(path, flags, &fs_err);
    if (!file) {
        return map_fs_error_to_resource(fs_err);
    }

    auto* impl = heap::kalloc_new<file_resource_impl>();
    if (!impl) {
        fs::close(file);
        return ERR_NOMEM;
    }
    impl->file = file;

    auto* obj = heap::kalloc_new<resource_object>();
    if (!obj) {
        heap::kfree_delete(impl);
        fs::close(file);
        return ERR_NOMEM;
    }

    obj->type = resource_type::FILE;
    obj->ops = &g_file_ops;
    obj->impl = impl;

    *out_obj = obj;
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE fs::file* get_file(resource_object* obj) {
    if (!obj || obj->type != resource_type::FILE || !obj->impl) {
        return nullptr;
    }
    auto* impl = static_cast<file_resource_impl*>(obj->impl);
    return impl->file;
}

} // namespace resource::file_provider
