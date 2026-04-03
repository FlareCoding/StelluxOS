#include "syscall/handlers/sys_memfd.h"
#include "resource/providers/shmem_resource_provider.h"
#include "resource/providers/file_provider.h"
#include "resource/handle_table.h"
#include "mm/shmem.h"
#include "mm/uaccess.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "sched/sched.h"
#include "sched/task.h"

namespace {

constexpr size_t MEMFD_NAME_MAX = 249;

inline int64_t map_fs_truncate_error(int32_t rc) {
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
constexpr uint32_t MFD_CLOEXEC = 0x0001u;
constexpr uint32_t MFD_ALLOWED = MFD_CLOEXEC;

} // namespace

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
    int32_t rc = resource::shmem_resource_provider::create_shmem_resource(flags, &obj);
    if (rc != resource::OK) {
        return syscall::ENOMEM;
    }

    resource::handle_t handle = -1;
    uint32_t rights = resource::RIGHT_READ | resource::RIGHT_WRITE;
    rc = resource::alloc_handle(
        &task->handles, obj, resource::resource_type::SHMEM, rights, &handle);
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(obj);
        return syscall::EMFILE;
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
        &task->handles, fd, resource::RIGHT_WRITE, &obj);
    if (rc != resource::HANDLE_OK) {
        return (rc == resource::HANDLE_ERR_ACCESS) ? syscall::EACCES : syscall::EBADF;
    }

    if (obj->type == resource::resource_type::SHMEM) {
        mm::shmem* backing = resource::shmem_resource_provider::get_shmem_backing(obj);
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
