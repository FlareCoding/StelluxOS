#include "syscall/handlers/sys_proc.h"
#include "resource/providers/proc_provider.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "exec/elf.h"
#include "mm/uaccess.h"
#include "fs/fstypes.h"
#include "common/string.h"

namespace {

__PRIVILEGED_CODE static const char* path_basename(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    return base;
}

__PRIVILEGED_CODE static int64_t map_elf_error(int32_t rc) {
    switch (rc) {
        case exec::ERR_FILE_OPEN:
            return syscall::ENOENT;
        case exec::ERR_NO_MEM:
        case exec::ERR_PT_CREATE:
        case exec::ERR_PAGE_ALLOC:
            return syscall::ENOMEM;
        case exec::ERR_FILE_READ:
            return syscall::EIO;
        default:
            return syscall::EINVAL;
    }
}

} // anonymous namespace

DEFINE_SYSCALL2(proc_create, u_path, u_argv) {
    (void)u_argv; // argv passing deferred to Phase 5

    char kpath[fs::PATH_MAX];
    int32_t copy_rc = mm::uaccess::copy_cstr_from_user(
        kpath, sizeof(kpath),
        reinterpret_cast<const char*>(u_path));
    if (copy_rc == mm::uaccess::ERR_NAMETOOLONG) {
        return syscall::ENAMETOOLONG;
    }
    if (copy_rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    exec::loaded_image loaded;
    int32_t elf_rc = exec::load_elf(kpath, &loaded);
    if (elf_rc != exec::OK) {
        return map_elf_error(elf_rc);
    }

    const char* name = path_basename(kpath);
    sched::task* child = sched::create_user_task(&loaded, name);
    if (!child) {
        exec::unload_elf(&loaded);
        return syscall::ENOMEM;
    }

    resource::resource_object* obj = nullptr;
    int32_t pr_rc = resource::proc_provider::create_proc_resource(child, &obj);
    if (pr_rc != resource::OK) {
        resource::proc_provider::destroy_unstarted_task(child);
        return syscall::ENOMEM;
    }

    sched::task* caller = sched::current();
    resource::handle_t handle = -1;
    int32_t h_rc = resource::alloc_handle(
        &caller->handles, obj, resource::resource_type::PROCESS, 0, &handle);
    if (h_rc != resource::HANDLE_OK) {
        resource::resource_release(obj);
        return syscall::EMFILE;
    }

    resource::resource_release(obj);
    return static_cast<int64_t>(handle);
}

DEFINE_SYSCALL1(proc_start, u_handle) {
    int32_t handle = static_cast<int32_t>(u_handle);

    sched::task* caller = sched::current();
    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &caller->handles, handle, 0, &obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    if (obj->type != resource::resource_type::PROCESS) {
        resource::resource_release(obj);
        return syscall::EBADF;
    }

    auto* pr = resource::proc_provider::get_proc_resource(obj);
    if (!pr) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
    if (!pr->child || pr->child->state != sched::TASK_STATE_CREATED) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    sched::enqueue(pr->child);
    sync::spin_unlock_irqrestore(pr->lock, irq);

    resource::resource_release(obj);
    return 0;
}

DEFINE_SYSCALL2(proc_wait, u_handle, u_exit_code_ptr) {
    (void)u_handle; (void)u_exit_code_ptr;
    return syscall::ENOSYS;
}

DEFINE_SYSCALL1(proc_detach, u_handle) {
    int32_t handle = static_cast<int32_t>(u_handle);

    sched::task* caller = sched::current();
    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &caller->handles, handle, 0, &obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    if (obj->type != resource::resource_type::PROCESS) {
        resource::resource_release(obj);
        return syscall::EBADF;
    }

    auto* pr = resource::proc_provider::get_proc_resource(obj);
    if (!pr) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
    pr->detached = true;
    sync::spin_unlock_irqrestore(pr->lock, irq);

    resource::resource_release(obj);
    resource::close(caller, handle);
    return 0;
}

DEFINE_SYSCALL2(proc_info, u_handle, u_info_ptr) {
    (void)u_handle; (void)u_info_ptr;
    return syscall::ENOSYS;
}
