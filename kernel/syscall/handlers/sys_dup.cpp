#include "syscall/handlers/sys_dup.h"
#include "resource/resource.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"

// Installs the object behind old_h at slot new_h, replacing any occupant.
// Per-handle flags carry over with CLOEXEC cleared unless set_cloexec.
static int64_t dup_to_slot(
    sched::task* task,
    resource::handle_t old_h,
    resource::handle_t new_h,
    bool set_cloexec
) {
    resource::resource_object* obj = nullptr;
    uint32_t flags = 0;
    uint32_t rights = 0;
    int32_t rc = resource::get_handle_object(
        task->handles, old_h, 0, &obj, &flags, &rights);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    rc = resource::install_handle_at(
        task->handles, new_h, obj, obj->type, rights);
    resource::resource_release(obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    flags &= ~resource::RESOURCE_HANDLE_CLOEXEC;
    if (set_cloexec) {
        flags |= resource::RESOURCE_HANDLE_CLOEXEC;
    }
    resource::set_handle_flags(task->handles, new_h, flags);

    return static_cast<int64_t>(new_h);
}

DEFINE_SYSCALL1(dup, u_oldfd) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    uint32_t flags = 0;
    uint32_t rights = 0;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(u_oldfd), 0,
        &obj, &flags, &rights);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    // alloc_handle scans from slot zero, giving the POSIX lowest-free fd
    resource::handle_t new_h = -1;
    rc = resource::alloc_handle(task->handles, obj, obj->type, rights, &new_h);
    resource::resource_release(obj);
    if (rc == resource::HANDLE_ERR_NOSPC) {
        return syscall::EMFILE;
    }
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    resource::set_handle_flags(
        task->handles, new_h, flags & ~resource::RESOURCE_HANDLE_CLOEXEC);

    return static_cast<int64_t>(new_h);
}

DEFINE_SYSCALL2(dup2, u_oldfd, u_newfd) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    auto old_h = static_cast<resource::handle_t>(u_oldfd);
    auto new_h = static_cast<resource::handle_t>(u_newfd);
    if (new_h < 0 || static_cast<uint32_t>(new_h) >= resource::MAX_TASK_HANDLES) {
        return syscall::EBADF;
    }

    // Equal descriptors are a no-op, but oldfd must still be valid
    if (old_h == new_h) {
        resource::resource_object* obj = nullptr;
        int32_t rc = resource::get_handle_object(task->handles, old_h, 0, &obj);
        if (rc != resource::HANDLE_OK) {
            return syscall::EBADF;
        }
        resource::resource_release(obj);
        return static_cast<int64_t>(new_h);
    }

    return dup_to_slot(task, old_h, new_h, false);
}

DEFINE_SYSCALL3(dup3, u_oldfd, u_newfd, u_flags) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    auto old_h = static_cast<resource::handle_t>(u_oldfd);
    auto new_h = static_cast<resource::handle_t>(u_newfd);
    if (old_h == new_h) {
        return syscall::EINVAL;
    }
    if (new_h < 0 || static_cast<uint32_t>(new_h) >= resource::MAX_TASK_HANDLES) {
        return syscall::EBADF;
    }
    if (u_flags & ~static_cast<uint64_t>(fs::O_CLOEXEC)) {
        return syscall::EINVAL;
    }

    return dup_to_slot(task, old_h, new_h, (u_flags & fs::O_CLOEXEC) != 0);
}
