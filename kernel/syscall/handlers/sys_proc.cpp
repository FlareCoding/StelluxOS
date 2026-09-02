#include "syscall/handlers/sys_proc.h"
#include "syscall/handlers/sys_error_map.h"
#include "resource/providers/proc_provider.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "signals/signal.h"
#include "dynpriv/dynpriv.h"
#include "exec/elf.h"
#include "mm/uaccess.h"
#include "mm/heap.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "fs/fstypes.h"
#include "common/string.h"

constexpr size_t MAX_PROC_ARGV_TOTAL = 3500;

namespace {

struct process_info {
    char name[256];
    int pid;
    int cpu;
};

// One bounded user string array copied into kernel storage
struct proc_string_array {
    char buf[MAX_PROC_ARGV_TOTAL];
    const char* ptrs[sched::MAX_ARG_STRINGS];
    int count;
};

// Everything proc_create copies out of user string arrays, heap-held
// because the buffers dwarf the system stack
struct proc_create_strings {
    proc_string_array argv;
    proc_string_array envp;
};

} // anonymous namespace

// Copies a NULL-terminated user pointer array of strings into arr. The
// user address stays an opaque integer until uaccess validates each access.
static int64_t copy_proc_string_array_from_user(
    proc_string_array& arr, uint64_t u_array
) {
    arr.count = 0;
    if (u_array == 0) {
        return 0;
    }

    size_t buf_offset = 0;
    for (size_t i = 0; i < sched::MAX_ARG_STRINGS; i++) {
        uintptr_t uptr = 0;
        int32_t rc = mm::uaccess::copy_from_user(
            &uptr,
            reinterpret_cast<const uintptr_t*>(u_array) + i,
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

        size_t cap = remaining < sched::MAX_ARG_STRLEN
                   ? remaining : sched::MAX_ARG_STRLEN;

        rc = mm::uaccess::copy_cstr_from_user(
            arr.buf + buf_offset, cap,
            reinterpret_cast<const char*>(uptr));
        if (rc == mm::uaccess::ERR_NAMETOOLONG) {
            return syscall::ENAMETOOLONG;
        }

        if (rc != mm::uaccess::OK) {
            return syscall::EFAULT;
        }

        size_t len = string::strnlen(arr.buf + buf_offset, cap);

        arr.ptrs[arr.count] = arr.buf + buf_offset;
        buf_offset += len + 1;
        arr.count++;
    }

    return 0;
}

// Copies the full proc_create string payload in one step
static int64_t copy_proc_create_strings_from_user(
    proc_create_strings& strs, uint64_t u_argv, uint64_t u_envp
) {
    int64_t rc = copy_proc_string_array_from_user(strs.argv, u_argv);
    if (rc != 0) {
        return rc;
    }

    return copy_proc_string_array_from_user(strs.envp, u_envp);
}

static int64_t map_elf_error(int32_t rc) {
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

DEFINE_SYSCALL3(proc_create, u_path, u_argv, u_envp) {
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

    auto* strs = heap::kalloc_new<proc_create_strings>();
    if (!strs) {
        return syscall::ENOMEM;
    }

    int64_t rc = copy_proc_create_strings_from_user(*strs, u_argv, u_envp);
    if (rc != 0) {
        heap::kfree_delete(strs);
        return rc;
    }

    exec::loaded_image loaded;
    int32_t elf_rc = exec::load_elf(kpath, &loaded, caller->cwd);
    if (elf_rc != exec::OK) {
        heap::kfree_delete(strs);
        return map_elf_error(elf_rc);
    }

    sched::task* child = sched::create_user_task(
        &loaded, kpath,
        strs->argv.count, strs->argv.count > 0 ? strs->argv.ptrs : nullptr,
        strs->envp.count, strs->envp.count > 0 ? strs->envp.ptrs : nullptr);
    heap::kfree_delete(strs);
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
            caller->handles, fd, 0, &fd_obj);
        if (fd_rc != resource::HANDLE_OK) continue;

        resource::handle_t child_fd = -1;
        resource::alloc_handle(
            child->handles, fd_obj,
            caller->handles->entries[static_cast<uint32_t>(fd)].type,
            caller->handles->entries[static_cast<uint32_t>(fd)].rights,
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
        caller->handles, obj, resource::resource_type::PROCESS, 0, &handle);
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
        caller->handles, handle, 0, &obj);
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
    if (!pr->child || pr->child->state.load_relaxed() != sched::TASK_STATE_CREATED) {
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
        caller->handles, handle, 0, &obj);
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
    if (pr->child && pr->child->state.load_relaxed() == sched::TASK_STATE_CREATED) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    while (!pr->exited && !signals::interrupt_pending(caller)) {
        irq = sync::wait(pr->wait_queue, pr->lock, irq);
    }

    if (!pr->exited) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(obj);
        return syscall::ERESTARTSYS;
    }

    int32_t child_wait_status = pr->wait_status;
    sync::spin_unlock_irqrestore(pr->lock, irq);

    if (u_exit_code_ptr != 0) {
        int32_t copy_rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_exit_code_ptr),
            &child_wait_status,
            sizeof(child_wait_status));
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
        caller->handles, handle, 0, &obj);
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
    int32_t rc = resource::get_handle_object(caller->handles, handle, 0, &obj);
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
        caller->handles, static_cast<resource::handle_t>(u_proc_handle), 0, &proc_obj);
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
    if (!pr->child || pr->child->state.load_relaxed() != sched::TASK_STATE_CREATED) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(proc_obj);
        return syscall::EINVAL;
    }

    resource::resource_object* res_obj = nullptr;
    uint32_t res_rights = 0;
    rc = resource::get_handle_object(
        caller->handles, static_cast<resource::handle_t>(u_resource_handle), 0,
        &res_obj, nullptr, &res_rights);
    if (rc != resource::HANDLE_OK) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(proc_obj);
        return syscall::EBADF;
    }

    rc = resource::install_handle_at(
        pr->child->handles, static_cast<resource::handle_t>(slot),
        res_obj, res_obj->type, res_rights);

    sync::spin_unlock_irqrestore(pr->lock, irq);
    resource::resource_release(res_obj);
    resource::resource_release(proc_obj);

    if (rc != resource::HANDLE_OK) {
        return syscall::EINVAL;
    }

    return 0;
}

DEFINE_SYSCALL1(proc_kill, u_handle) {
    int32_t handle = static_cast<int32_t>(u_handle);
    sched::task* caller = sched::current();
    if (!caller) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(caller->handles, handle, 0, &obj);
    if (rc != resource::HANDLE_OK || !obj) {
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

    // The reference taken under pr->lock keeps the child reclaim-safe
    // after the lock drops, even if it exits and is reaped concurrently
    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
    bool exited = pr->exited;

    rc::strong_ref<sched::task> child;
    if (pr->child && !exited) {
        child = sched::task_ref(pr->child);
    }

    sync::spin_unlock_irqrestore(pr->lock, irq);

    if (!child) {
        resource::resource_release(obj);
        return exited ? 0 : syscall::EINVAL;
    }

    RUN_ELEVATED(sched::force_wake_for_kill(child.ptr()));

    resource::resource_release(obj);
    return 0;
}

DEFINE_SYSCALL4(proc_create_thread, u_entry, u_arg, u_stack_top, u_name) {
    sched::task* caller = sched::current();
    if (!caller) {
        return syscall::EIO;
    }

    if (u_stack_top == 0) {
        return syscall::EINVAL;
    }

    char kname[sched::TASK_NAME_MAX];
    if (u_name != 0) {
        int32_t copy_rc = mm::uaccess::copy_cstr_from_user(
            kname, sizeof(kname),
            reinterpret_cast<const char*>(u_name));
        if (copy_rc == mm::uaccess::ERR_NAMETOOLONG) {
            return syscall::ENAMETOOLONG;
        }

        if (copy_rc != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    } else {
        kname[0] = '\0';
    }

    sched::task* child = sched::create_user_thread(
        caller, u_entry, u_arg, u_stack_top, kname);
    if (!child) {
        return syscall::ENOMEM;
    }

    resource::resource_object* obj = nullptr;
    int32_t pr_rc = resource::proc_provider::create_proc_resource(child, &obj);
    if (pr_rc != resource::OK) {
        resource::proc_provider::destroy_unstarted_task(child);
        return syscall::ENOMEM;
    }

    resource::handle_t handle = -1;
    int32_t h_rc = resource::alloc_handle(
        caller->handles, obj, resource::resource_type::PROCESS, 0, &handle);
    if (h_rc != resource::HANDLE_OK) {
        resource::resource_release(obj);
        return syscall::EMFILE;
    }

    resource::resource_release(obj);
    return static_cast<int64_t>(handle);
}
