#include "resource/resource.h"
#include "resource/providers/file_provider.h"
#include "resource/providers/shm_provider.h"
#include "sched/task.h"
#include "fs/fstypes.h"
#include "mm/heap.h"

namespace resource {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void resource_object::ref_destroy(resource_object* self) {
    if (!self) {
        return;
    }

    if (self->ops && self->ops->close) {
        self->ops->close(self);
    }
    heap::kfree_delete(self);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void resource_add_ref(resource_object* obj) {
    if (!obj) {
        return;
    }
    obj->add_ref();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void resource_release(resource_object* obj) {
    if (!obj) {
        return;
    }
    if (obj->release()) {
        resource_object::ref_destroy(obj);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_task_handles(sched::task* task) {
    if (!task) {
        return;
    }
    init_handle_table(&task->handles);
}

__PRIVILEGED_CODE static bool valid_open_flags(uint32_t flags) {
    uint32_t mode = flags & fs::ACCESS_MODE_MASK;
    return mode == fs::O_RDONLY || mode == fs::O_WRONLY || mode == fs::O_RDWR;
}

__PRIVILEGED_CODE static uint32_t normalize_open_flags(uint32_t flags) {
    return flags & (fs::ACCESS_MODE_MASK | fs::O_CREAT | fs::O_EXCL | fs::O_TRUNC | fs::O_APPEND);
}

__PRIVILEGED_CODE static uint32_t rights_from_open_flags(uint32_t flags) {
    uint32_t rights = 0;
    uint32_t mode = flags & fs::ACCESS_MODE_MASK;
    if (mode == fs::O_RDONLY || mode == fs::O_RDWR) {
        rights |= RIGHT_READ;
    }
    if (mode == fs::O_WRONLY || mode == fs::O_RDWR) {
        rights |= RIGHT_WRITE;
    }
    return rights;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t open(
    sched::task* owner,
    const char* kpath,
    uint32_t flags,
    handle_t* out_handle
) {
    if (!owner || !kpath || !out_handle) {
        return ERR_INVAL;
    }
    if (!valid_open_flags(flags)) {
        return ERR_INVAL;
    }
    uint32_t fs_flags = normalize_open_flags(flags);

    resource_object* obj = nullptr;
    int32_t rc;
    resource_type rtype;

    if (shm_provider::is_shm_path(kpath)) {
        rc = shm_provider::open_shm_resource(kpath, fs_flags, &obj);
        rtype = resource_type::SHMEM;
    } else {
        rc = file_provider::open_file_resource(kpath, fs_flags, &obj);
        rtype = resource_type::FILE;
    }
    if (rc != OK) {
        return rc;
    }

    uint32_t rights = rights_from_open_flags(fs_flags);
    rc = alloc_handle(&owner->handles, obj, rtype, rights, out_handle);
    if (rc != HANDLE_OK) {
        resource_release(obj);
        return (rc == HANDLE_ERR_NOSPC) ? ERR_TABLEFULL : ERR_IO;
    }

    // Table now owns one reference.
    resource_release(obj);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t read(
    sched::task* owner,
    handle_t handle,
    void* kdst,
    size_t count
) {
    if (!owner || !kdst) {
        return ERR_INVAL;
    }

    resource_object* obj = nullptr;
    uint32_t handle_flags = 0;
    int32_t rc = get_handle_object(&owner->handles, handle, RIGHT_READ, &obj, &handle_flags);
    if (rc != HANDLE_OK) {
        return (rc == HANDLE_ERR_ACCESS) ? ERR_ACCESS : ERR_BADF;
    }

    ssize_t result = ERR_UNSUP;
    if (obj->ops && obj->ops->read) {
        result = obj->ops->read(obj, kdst, count, handle_flags);
    }

    resource_release(obj);
    return result;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t write(
    sched::task* owner,
    handle_t handle,
    const void* ksrc,
    size_t count
) {
    if (!owner || !ksrc) {
        return ERR_INVAL;
    }

    resource_object* obj = nullptr;
    uint32_t handle_flags = 0;
    int32_t rc = get_handle_object(&owner->handles, handle, RIGHT_WRITE, &obj, &handle_flags);
    if (rc != HANDLE_OK) {
        return (rc == HANDLE_ERR_ACCESS) ? ERR_ACCESS : ERR_BADF;
    }

    ssize_t result = ERR_UNSUP;
    if (obj->ops && obj->ops->write) {
        result = obj->ops->write(obj, ksrc, count, handle_flags);
    }

    resource_release(obj);
    return result;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t ioctl(
    sched::task* owner,
    handle_t handle,
    uint32_t cmd,
    uint64_t arg
) {
    if (!owner) {
        return ERR_INVAL;
    }

    resource_object* obj = nullptr;
    uint32_t handle_flags = 0;
    int32_t rc = get_handle_object(&owner->handles, handle, 0, &obj, &handle_flags);
    if (rc != HANDLE_OK) {
        return ERR_BADF;
    }

    int32_t result = ERR_UNSUP;
    if (obj->ops && obj->ops->ioctl) {
        result = obj->ops->ioctl(obj, cmd, arg);
    }

    resource_release(obj);
    return result;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t close(
    sched::task* owner,
    handle_t handle
) {
    if (!owner) {
        return ERR_INVAL;
    }

    resource_object* obj = nullptr;
    int32_t rc = remove_handle(&owner->handles, handle, &obj);
    if (rc != HANDLE_OK) {
        return ERR_BADF;
    }

    resource_release(obj);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void close_all(sched::task* owner) {
    if (!owner) {
        return;
    }

    for (uint32_t i = 0; i < MAX_TASK_HANDLES; i++) {
        resource_object* obj = nullptr;
        int32_t rc = remove_handle(&owner->handles, static_cast<handle_t>(i), &obj);
        if (rc == HANDLE_OK) {
            resource_release(obj);
        }
    }
}

} // namespace resource
