#include "syscall/handlers/sys_memfd.h"
#include "syscall/handlers/sys_error_map.h"
#include "resource/providers/shmem_provider.h"
#include "resource/providers/file_provider.h"
#include "resource/handle_table.h"
#include "mm/shmem.h"
#include "mm/uaccess.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "sched/sched.h"
#include "sched/task.h"

constexpr size_t MEMFD_NAME_MAX = 249;
constexpr uint32_t FALLOC_FL_ALLOCATE_RANGE = 0;

static inline int64_t map_fs_truncate_error(int32_t rc) {
    switch (rc) {
        case fs::ERR_INVAL:
            return syscall::EINVAL;
        case fs::ERR_NOMEM:
            return syscall::ENOMEM;
        case fs::ERR_NOSYS:
            return syscall::EINVAL;
        default:
            return syscall::EIO;
    }
}

static inline int64_t map_fs_allocate_error(int32_t rc) {
    switch (rc) {
        case fs::ERR_NOMEM:
            return syscall::ENOSPC;
        case fs::ERR_NOSYS:
            return syscall::EOPNOTSUPP;
        default:
            return syscall::EIO;
    }
}

constexpr uint32_t MFD_CLOEXEC = 0x0001u;
constexpr uint32_t MFD_ALLOWED = MFD_CLOEXEC;

DEFINE_SYSCALL2(memfd_create, u_name, u_flags) {
    uint32_t flags = static_cast<uint32_t>(u_flags);
    if (flags & ~MFD_ALLOWED) {
        return syscall::EINVAL;
    }

    char kname[MEMFD_NAME_MAX + 1];
    if (u_name != 0) {
        int32_t rc = mm::uaccess::copy_cstr_from_user(
            kname, sizeof(kname),
            reinterpret_cast<const char*>(u_name));
        if (rc == mm::uaccess::ERR_NAMETOOLONG) {
            return syscall::ENAMETOOLONG;
        }

        if (rc != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::ENOMEM;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::shmem_provider::create_shmem_resource(flags, &obj);
    if (rc != resource::OK) {
        return syscall::ENOMEM;
    }

    resource::handle_t handle = -1;
    uint32_t rights = resource::RIGHT_READ | resource::RIGHT_WRITE;
    rc = resource::alloc_task_handle(
        task, obj, resource::resource_type::SHMEM, rights, &handle);
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(obj);
        return syscall::error_map::map_handle_alloc_error(rc);
    }

    resource::resource_release(obj);
    return static_cast<int64_t>(handle);
}

DEFINE_SYSCALL2(ftruncate, fd_val, length) {
    int32_t fd = static_cast<int32_t>(fd_val);
    int64_t signed_len = static_cast<int64_t>(length);
    if (signed_len < 0) {
        return syscall::EINVAL;
    }

    size_t new_size = static_cast<size_t>(signed_len);

    sched::task* task = sched::current();
    if (!task) {
        return syscall::ENOMEM;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, fd, resource::RIGHT_WRITE, &obj);
    if (rc != resource::HANDLE_OK) {
        return (rc == resource::HANDLE_ERR_ACCESS) ? syscall::EACCES : syscall::EBADF;
    }

    if (obj->type == resource::resource_type::SHMEM) {
        mm::shmem* backing = resource::shmem_provider::get_shmem_backing(obj);
        if (!backing) {
            resource::resource_release(obj);
            return syscall::EINVAL;
        }

        sync::mutex_lock(backing->lock);
        int32_t resize_rc = mm::shmem_resize_locked(backing, new_size);
        sync::mutex_unlock(backing->lock);

        resource::resource_release(obj);
        return (resize_rc != mm::SHMEM_OK) ? syscall::ENOMEM : 0;
    }

    if (obj->type == resource::resource_type::FILE) {
        fs::file* f = resource::file_provider::get_file(obj);
        if (!f || !f->get_node()) {
            resource::resource_release(obj);
            return syscall::EINVAL;
        }

        int32_t trunc_rc = f->get_node()->truncate(new_size);
        resource::resource_release(obj);
        return (trunc_rc != 0) ? map_fs_truncate_error(trunc_rc) : 0;
    }

    resource::resource_release(obj);
    return syscall::EINVAL;
}

__PRIVILEGED_CODE static int64_t grow_memory_file(resource::resource_object* obj, uint64_t min_size) {
    mm::shmem* backing = resource::shmem_provider::get_shmem_backing(obj);
    if (!backing) {
        return syscall::EINVAL;
    }

    sync::mutex_lock(backing->lock);
    int32_t grow_rc = mm::shmem_grow_locked(backing, min_size);
    sync::mutex_unlock(backing->lock);

    return (grow_rc != mm::SHMEM_OK) ? syscall::ENOSPC : 0;
}

// The order of these checks decides which error a call that fails several of them reports
__PRIVILEGED_CODE static int64_t allocate_range(
    resource::resource_object* obj, uint32_t rights, uint32_t mode, int64_t offset, int64_t length
) {
    if (offset < 0 || length <= 0) {
        return syscall::EINVAL;
    }

    if (mode != FALLOC_FL_ALLOCATE_RANGE) {
        return syscall::EOPNOTSUPP;
    }

    if (!(rights & resource::RIGHT_WRITE)) {
        return syscall::EBADF;
    }

    if (obj->type == resource::resource_type::PIPE) {
        return syscall::ESPIPE;
    }

    fs::file* f = resource::file_provider::get_file(obj);
    fs::node* node = f ? f->get_node() : nullptr;
    if (node && node->type() == fs::node_type::directory) {
        return syscall::EISDIR;
    }

    bool is_memory_file = obj->type == resource::resource_type::SHMEM;
    bool is_regular_file = node && node->type() == fs::node_type::regular;
    if (!is_memory_file && !is_regular_file) {
        return syscall::ENODEV;
    }

    if (length > fs::MAX_FILE_OFFSET - offset) {
        return syscall::EFBIG;
    }

    if (is_memory_file) {
        return grow_memory_file(obj, static_cast<uint64_t>(offset + length));
    }

    int32_t allocate_rc = node->allocate(static_cast<uint64_t>(offset), static_cast<uint64_t>(length));

    return (allocate_rc != fs::OK) ? map_fs_allocate_error(allocate_rc) : 0;
}

DEFINE_SYSCALL4(fallocate, fd, mode, offset, length) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::ENOMEM;
    }

    resource::resource_object* obj = nullptr;
    uint32_t rights = 0;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd), 0, &obj, nullptr, &rights);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    int64_t result = allocate_range(obj, rights, static_cast<uint32_t>(mode),
                                    static_cast<int64_t>(offset), static_cast<int64_t>(length));
    resource::resource_release(obj);

    return result;
}
