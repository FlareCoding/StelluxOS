#include "resource/providers/shmem_resource_provider.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "mm/shmem.h"
#include "rc/strong_ref.h"

namespace resource::shmem_resource_provider {

struct shmem_resource_impl {
    rc::strong_ref<mm::shmem> backing;
    size_t offset;

    ~shmem_resource_impl() = default;
};

static ssize_t shmem_resource_read(
    resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    (void)flags;
    if (!obj || !kdst) {
        return ERR_INVAL;
    }

    ssize_t result = ERR_INVAL;
    RUN_ELEVATED({
        if (!obj->impl) {
            result = ERR_INVAL;
        } else {
            auto* impl = static_cast<shmem_resource_impl*>(obj->impl);
            if (impl->backing) {
                result = mm::shmem_read(impl->backing.ptr(), impl->offset, kdst, count);
                if (result > 0) {
                    impl->offset += static_cast<size_t>(result);
                }
            }
        }
    });
    return result;
}

static ssize_t shmem_resource_write(
    resource_object* obj, const void* ksrc, size_t count, uint32_t flags
) {
    (void)flags;
    if (!obj || !ksrc) {
        return ERR_INVAL;
    }

    ssize_t result = ERR_INVAL;
    RUN_ELEVATED({
        if (!obj->impl) {
            result = ERR_INVAL;
        } else {
            auto* impl = static_cast<shmem_resource_impl*>(obj->impl);
            if (impl->backing) {
                result = mm::shmem_write(impl->backing.ptr(), impl->offset, ksrc, count);
                if (result > 0) {
                    impl->offset += static_cast<size_t>(result);
                }
            }
        }
    });
    return result;
}

static void shmem_resource_close(resource_object* obj) {
    if (!obj) {
        return;
    }
    RUN_ELEVATED({
        if (obj->impl) {
            auto* impl = static_cast<shmem_resource_impl*>(obj->impl);
            heap::kfree_delete(impl);
            obj->impl = nullptr;
        }
    });
}

static const resource_ops g_shmem_resource_ops = {
    shmem_resource_read,
    shmem_resource_write,
    shmem_resource_close,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

int32_t create_shmem_resource(
    uint32_t flags,
    resource_object** out_obj
) {
    (void)flags;
    if (!out_obj) {
        return ERR_INVAL;
    }

    mm::shmem* backing = mm::shmem_create(0);
    if (!backing) {
        return ERR_NOMEM;
    }

    int32_t result = OK;
    resource_object* obj = nullptr;
    shmem_resource_impl* impl = nullptr;
    bool backing_still_owned = true;
    RUN_ELEVATED({
        impl = heap::kalloc_new<shmem_resource_impl>();
        if (!impl) {
            result = ERR_NOMEM;
        } else {
            impl->backing = rc::strong_ref<mm::shmem>::adopt(backing);
            impl->offset = 0;
            backing_still_owned = false;

            obj = heap::kalloc_new<resource_object>();
            if (!obj) {
                heap::kfree_delete(impl);
                impl = nullptr;
                result = ERR_NOMEM;
            }
        }

        if (result == OK) {
            obj->type = resource_type::SHMEM;
            obj->ops = &g_shmem_resource_ops;
            obj->impl = impl;
            *out_obj = obj;
        }
    });

    if (result != OK && backing_still_owned) {
        mm::shmem::ref_destroy(backing);
    }
    return result;
}

int32_t create_shmem_resource_with_backing(
    mm::shmem* backing,
    uint32_t flags,
    resource_object** out_obj
) {
    (void)flags;
    if (!backing || !out_obj) {
        return ERR_INVAL;
    }

    int32_t result = OK;
    resource_object* obj = nullptr;
    RUN_ELEVATED({
        auto* impl = heap::kalloc_new<shmem_resource_impl>();
        if (!impl) {
            result = ERR_NOMEM;
        } else {
            backing->add_ref();
            impl->backing = rc::strong_ref<mm::shmem>::adopt(backing);
            impl->offset = 0;

            obj = heap::kalloc_new<resource_object>();
            if (!obj) {
                heap::kfree_delete(impl);
                result = ERR_NOMEM;
            } else {
                obj->type = resource_type::SHMEM;
                obj->ops = &g_shmem_resource_ops;
                obj->impl = impl;
                *out_obj = obj;
            }
        }
    });
    return result;
}

mm::shmem* get_shmem_backing(resource_object* obj) {
    mm::shmem* backing = nullptr;
    RUN_ELEVATED({
        if (obj && obj->type == resource_type::SHMEM && obj->impl) {
            auto* impl = static_cast<shmem_resource_impl*>(obj->impl);
            backing = impl->backing.ptr();
        }
    });
    return backing;
}

} // namespace resource::shmem_resource_provider
