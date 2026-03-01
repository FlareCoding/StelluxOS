#include "resource/resource.h"
#include "resource/providers/file_provider.h"
#include "sched/task.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "mm/heap.h"

namespace resource {

__PRIVILEGED_CODE static int32_t map_fs_error(int32_t fs_err) {
    switch (fs_err) {
        case fs::OK:
            return OK;
        case fs::ERR_NOENT:
            return ERR_NOENT;
        case fs::ERR_NOMEM:
            return ERR_NOMEM;
        case fs::ERR_BADF:
            return ERR_BADF;
        case fs::ERR_NOSYS:
            return ERR_UNSUP;
        default:
            return ERR_IO;
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void resource_add_ref(resource_object* obj) {
    if (!obj) {
        return;
    }
    __atomic_fetch_add(&obj->refcount, 1, __ATOMIC_RELAXED);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void resource_release(resource_object* obj) {
    if (!obj) {
        return;
    }
    uint32_t old = __atomic_fetch_sub(&obj->refcount, 1, __ATOMIC_RELEASE);
    if (old == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (obj->ops && obj->ops->close) {
            obj->ops->close(obj);
        }
        heap::kfree_delete(obj);
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
    constexpr uint32_t ALLOWED_FLAGS = fs::O_CREAT | fs::O_TRUNC | fs::O_APPEND | fs::ACCESS_MODE_MASK;
    if ((flags & ~ALLOWED_FLAGS) != 0) {
        return false;
    }
    uint32_t mode = flags & fs::ACCESS_MODE_MASK;
    return mode == fs::O_RDONLY || mode == fs::O_WRONLY || mode == fs::O_RDWR;
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

    resource_object* obj = nullptr;
    int32_t rc = file_provider::open_file_resource(kpath, flags, &obj);
    if (rc != OK) {
        return rc;
    }

    uint32_t rights = rights_from_open_flags(flags);
    rc = alloc_handle(&owner->handles, obj, resource_type::FILE, rights, out_handle);
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
    int32_t rc = get_handle_object(&owner->handles, handle, RIGHT_READ, &obj);
    if (rc != HANDLE_OK) {
        return (rc == HANDLE_ERR_ACCESS) ? ERR_ACCESS : ERR_BADF;
    }

    ssize_t result = ERR_UNSUP;
    if (obj->ops && obj->ops->read) {
        result = obj->ops->read(obj, kdst, count);
    }

    resource_release(obj);

    if (result < 0) {
        return map_fs_error(static_cast<int32_t>(result));
    }
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
    int32_t rc = get_handle_object(&owner->handles, handle, RIGHT_WRITE, &obj);
    if (rc != HANDLE_OK) {
        return (rc == HANDLE_ERR_ACCESS) ? ERR_ACCESS : ERR_BADF;
    }

    ssize_t result = ERR_UNSUP;
    if (obj->ops && obj->ops->write) {
        result = obj->ops->write(obj, ksrc, count);
    }

    resource_release(obj);

    if (result < 0) {
        return map_fs_error(static_cast<int32_t>(result));
    }
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
