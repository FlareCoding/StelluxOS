#include "resource/providers/file_provider.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "mm/heap.h"

namespace resource::file_provider {

struct file_resource_impl {
    fs::file* file;
};

__PRIVILEGED_CODE static ssize_t file_read(resource_object* obj, void* kdst, size_t count) {
    if (!obj || !obj->impl || !kdst) {
        return ERR_INVAL;
    }
    auto* impl = static_cast<file_resource_impl*>(obj->impl);
    return fs::read(impl->file, kdst, count);
}

__PRIVILEGED_CODE static ssize_t file_write(resource_object* obj, const void* ksrc, size_t count) {
    if (!obj || !obj->impl || !ksrc) {
        return ERR_INVAL;
    }
    auto* impl = static_cast<file_resource_impl*>(obj->impl);
    return fs::write(impl->file, ksrc, count);
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

static const resource_ops g_file_ops = {
    file_read,
    file_write,
    file_close,
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

    fs::file* file = fs::open(path, flags);
    if (!file) {
        return ERR_NOENT;
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
    obj->refcount = 1;

    *out_obj = obj;
    return OK;
}

} // namespace resource::file_provider
