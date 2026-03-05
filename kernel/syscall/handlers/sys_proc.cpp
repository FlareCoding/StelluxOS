#include "syscall/handlers/sys_proc.h"
#include "syscall/handlers/sys_error_map.h"
#include "resource/providers/proc_provider.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "exec/elf.h"
#include "mm/uaccess.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "fs/fstypes.h"
#include "common/string.h"

namespace {

struct process_info {
    char name[256];
    int pid;
    int cpu;
};

constexpr uint32_t MAX_PROC_ARGC = 64;
constexpr size_t MAX_PROC_ARG_LEN = 256;
constexpr size_t MAX_PROC_ARGV_TOTAL = 3500;

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
    sched::task* caller = sched::current();
    if (!caller) {
        return syscall::EIO;
    }

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

    char kargv_buf[MAX_PROC_ARGV_TOTAL];
    const char* kargv_ptrs[MAX_PROC_ARGC];
    int kargc = 0;

    if (u_argv != 0) {
        size_t buf_offset = 0;
        for (uint32_t i = 0; i < MAX_PROC_ARGC; i++) {
            uintptr_t uptr = 0;
            int32_t rc = mm::uaccess::copy_from_user(
                &uptr,
                reinterpret_cast<const uintptr_t*>(u_argv) + i,
                sizeof(uptr));
            if (rc != mm::uaccess::OK) {
                return syscall::EFAULT;
            }
            if (uptr == 0) {
                break;
            }

            size_t remaining = MAX_PROC_ARGV_TOTAL - buf_offset;
            if (remaining == 0) {
                return syscall::ENAMETOOLONG;
            }
            size_t cap = remaining < MAX_PROC_ARG_LEN ? remaining : MAX_PROC_ARG_LEN;

            rc = mm::uaccess::copy_cstr_from_user(
                kargv_buf + buf_offset,
                cap,
                reinterpret_cast<const char*>(uptr));
            if (rc == mm::uaccess::ERR_NAMETOOLONG) {
                return syscall::ENAMETOOLONG;
            }
            if (rc != mm::uaccess::OK) {
                return syscall::EFAULT;
            }

            size_t len = string::strnlen(kargv_buf + buf_offset, cap);

            kargv_ptrs[kargc] = kargv_buf + buf_offset;
            buf_offset += len + 1;
            kargc++;
        }
    }

    exec::loaded_image loaded;
    int32_t elf_rc = exec::load_elf(kpath, &loaded);
    if (elf_rc != exec::OK) {
        return map_elf_error(elf_rc);
    }

    const char* name = path_basename(kpath);
    sched::task* child = sched::create_user_task(
        &loaded, name, kargc, kargc > 0 ? kargv_ptrs : nullptr);
    if (!child) {
        exec::unload_elf(&loaded);
        return syscall::ENOMEM;
    }

    fs::node* inherited_cwd = nullptr;
    int32_t cwd_rc = fs::OK;
    if (caller->cwd) {
        caller->cwd->add_ref();
        inherited_cwd = caller->cwd;
    } else {
        cwd_rc = fs::lookup("/", &inherited_cwd);
    }
    if (cwd_rc != fs::OK || !inherited_cwd) {
        resource::proc_provider::destroy_unstarted_task(child);
        return syscall::error_map::map_fs_error(cwd_rc);
    }
    child->cwd = inherited_cwd;

    for (resource::handle_t fd = 0; fd < 3; fd++) {
        resource::resource_object* fd_obj = nullptr;
        int32_t fd_rc = resource::get_handle_object(
            &caller->handles, fd, 0, &fd_obj);
        if (fd_rc != resource::HANDLE_OK) continue;

        resource::handle_t child_fd = -1;
        resource::alloc_handle(
            &child->handles, fd_obj,
            caller->handles.entries[static_cast<uint32_t>(fd)].type,
            caller->handles.entries[static_cast<uint32_t>(fd)].rights,
            &child_fd);

        resource::resource_release(fd_obj);
    }

    resource::resource_object* obj = nullptr;
    int32_t pr_rc = resource::proc_provider::create_proc_resource(child, &obj);
    if (pr_rc != resource::OK) {
        resource::proc_provider::destroy_unstarted_task(child);
        return syscall::ENOMEM;
    }

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
    if (pr->child && pr->child->state == sched::TASK_STATE_CREATED) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(obj);
        return syscall::EINVAL;
    }
    while (!pr->exited) {
        irq = sync::wait(pr->wait_queue, pr->lock, irq);
        sched::terminate_if_requested();
    }
    int32_t child_exit_code = pr->exit_code;
    sync::spin_unlock_irqrestore(pr->lock, irq);

    if (u_exit_code_ptr != 0) {
        int32_t copy_rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_exit_code_ptr),
            &child_exit_code,
            sizeof(child_exit_code));
        if (copy_rc != mm::uaccess::OK) {
            resource::resource_release(obj);
            return syscall::EFAULT;
        }
    }

    resource::resource_release(obj);
    resource::close(caller, handle);
    return 0;
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
    if (u_info_ptr == 0) {
        return syscall::EFAULT;
    }

    int32_t handle = static_cast<int32_t>(u_handle);
    sched::task* caller = sched::current();
    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(&caller->handles, handle, 0, &obj);
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

    process_info kinfo = {};
    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
    if (!pr->child) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(obj);
        return syscall::ESRCH;
    }
    string::memcpy(kinfo.name, pr->child->name,
                   string::strnlen(pr->child->name, sched::TASK_NAME_MAX - 1) + 1);
    kinfo.pid = static_cast<int>(pr->child->tid);
    kinfo.cpu = static_cast<int>(pr->child->exec.cpu);
    sync::spin_unlock_irqrestore(pr->lock, irq);

    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_info_ptr), &kinfo, sizeof(kinfo));
    if (copy_rc != mm::uaccess::OK) {
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    resource::resource_release(obj);
    return 0;
}

DEFINE_SYSCALL3(proc_set_handle, u_proc_handle, u_slot, u_resource_handle) {
    sched::task* caller = sched::current();
    if (!caller) return syscall::EIO;

    int32_t slot = static_cast<int32_t>(u_slot);
    if (slot < 0 || static_cast<uint32_t>(slot) >= resource::MAX_TASK_HANDLES) {
        return syscall::EINVAL;
    }

    resource::resource_object* proc_obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &caller->handles, static_cast<resource::handle_t>(u_proc_handle), 0, &proc_obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    if (proc_obj->type != resource::resource_type::PROCESS) {
        resource::resource_release(proc_obj);
        return syscall::EBADF;
    }

    auto* pr = resource::proc_provider::get_proc_resource(proc_obj);
    if (!pr) {
        resource::resource_release(proc_obj);
        return syscall::EINVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
    if (!pr->child || pr->child->state != sched::TASK_STATE_CREATED) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(proc_obj);
        return syscall::EINVAL;
    }

    resource::resource_object* res_obj = nullptr;
    uint32_t res_rights = 0;
    rc = resource::get_handle_object(
        &caller->handles, static_cast<resource::handle_t>(u_resource_handle), 0,
        &res_obj, nullptr, &res_rights);
    if (rc != resource::HANDLE_OK) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(proc_obj);
        return syscall::EBADF;
    }

    if (res_obj->type == resource::resource_type::PROCESS) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(res_obj);
        resource::resource_release(proc_obj);
        return syscall::EINVAL;
    }

    rc = resource::install_handle_at(
        &pr->child->handles, static_cast<resource::handle_t>(slot),
        res_obj, res_obj->type, res_rights);

    sync::spin_unlock_irqrestore(pr->lock, irq);
    resource::resource_release(res_obj);
    resource::resource_release(proc_obj);

    if (rc != resource::HANDLE_OK) {
        return syscall::EINVAL;
    }
    return 0;
}
