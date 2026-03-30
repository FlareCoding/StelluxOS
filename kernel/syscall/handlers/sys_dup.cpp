#include "syscall/handlers/sys_dup.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "sched/sched.h"
#include "sched/task.h"

/**
 * dup(oldfd) -> new fd (lowest available)
 */
DEFINE_SYSCALL1(dup, oldfd) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::handle_t old_h = static_cast<resource::handle_t>(oldfd);
    resource::resource_object* obj = nullptr;
    uint32_t rights = 0;
    int32_t rc = resource::get_handle_object(
        &task->handles, old_h, 0, &obj, nullptr, &rights);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    // Determine the type from the original handle entry
    resource::resource_type type = obj->type;

    resource::handle_t new_h = -1;
    rc = resource::alloc_handle(&task->handles, obj, type, rights, &new_h);
    resource::resource_release(obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EMFILE;
    }

    return static_cast<int64_t>(new_h);
}

/**
 * dup2(oldfd, newfd) -> newfd on success
 *
 * If oldfd == newfd, return newfd (no-op).
 * If newfd is occupied, close it first (via install_handle_at which replaces).
 */
DEFINE_SYSCALL2(dup2, oldfd, newfd) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::handle_t old_h = static_cast<resource::handle_t>(oldfd);
    resource::handle_t new_h = static_cast<resource::handle_t>(newfd);

    if (new_h < 0 || static_cast<uint32_t>(new_h) >= resource::MAX_TASK_HANDLES) {
        return syscall::EBADF;
    }

    // If oldfd == newfd, verify oldfd is valid and return newfd
    if (old_h == new_h) {
        resource::resource_object* obj = nullptr;
        int32_t rc = resource::get_handle_object(&task->handles, old_h, 0, &obj);
        if (rc != resource::HANDLE_OK) return syscall::EBADF;
        resource::resource_release(obj);
        return static_cast<int64_t>(new_h);
    }

    resource::resource_object* obj = nullptr;
    uint32_t rights = 0;
    int32_t rc = resource::get_handle_object(
        &task->handles, old_h, 0, &obj, nullptr, &rights);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    resource::resource_type type = obj->type;

    rc = resource::install_handle_at(&task->handles, new_h, obj, type, rights);
    resource::resource_release(obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    return static_cast<int64_t>(new_h);
}

/**
 * dup3(oldfd, newfd, flags) -> newfd on success
 *
 * Like dup2, but oldfd != newfd is required (returns EINVAL if equal).
 * flags is currently ignored (O_CLOEXEC not relevant without exec).
 */
DEFINE_SYSCALL3(dup3, oldfd, newfd, flags) {
    (void)flags;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::handle_t old_h = static_cast<resource::handle_t>(oldfd);
    resource::handle_t new_h = static_cast<resource::handle_t>(newfd);

    if (old_h == new_h) return syscall::EINVAL;
    if (new_h < 0 || static_cast<uint32_t>(new_h) >= resource::MAX_TASK_HANDLES) {
        return syscall::EBADF;
    }

    resource::resource_object* obj = nullptr;
    uint32_t rights = 0;
    int32_t rc = resource::get_handle_object(
        &task->handles, old_h, 0, &obj, nullptr, &rights);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    resource::resource_type type = obj->type;

    rc = resource::install_handle_at(&task->handles, new_h, obj, type, rights);
    resource::resource_release(obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    return static_cast<int64_t>(new_h);
}
