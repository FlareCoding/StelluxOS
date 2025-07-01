#include <syscall/handlers/sys_proc.h>
#include <process/process.h>
#include <process/elf/elf64_loader.h>
#include <sched/sched.h>
#include <core/klog.h>

// Userland process creation flags
typedef enum {
    PROC_NONE           = 0 << 0,  // Invalid / empty flags
    PROC_SHARE_ENV      = 1 << 0,  // Share environment with parent
    PROC_COPY_ENV       = 1 << 1,  // Copy parent's environment
    PROC_NEW_ENV        = 1 << 2,  // Create new environment
    PROC_CAN_ELEVATE    = 1 << 3,  // Process's ASID is added to the dynamic privilege whitelist
} userland_proc_flags_t;

DECLARE_SYSCALL_HANDLER(getpid) {
    pid_t pid = current->get_core()->identity.pid;
        
    SYSCALL_TRACE("getpid() = %llu\n", pid);    
    return static_cast<long>(pid);
}

DECLARE_SYSCALL_HANDLER(exit) {
    current->get_core()->exit_code = arg1;
    SYSCALL_TRACE("exit(%llu) = ?\n", arg1);
    sched::exit_process();

    return 0;
}

DECLARE_SYSCALL_HANDLER(exit_group) {
    current->get_core()->exit_code = arg1;
    SYSCALL_TRACE("exit_group(%llu) = ?\n", arg1);
    sched::exit_process();

    return 0;
}

DECLARE_SYSCALL_HANDLER(proc_create) {
    // arg1 = path to executable
    // arg2 = process creation flags
    // arg3 = access rights
    // arg4 = handle flags
    // arg5 = pointer to proc_info struct
    const char* path = reinterpret_cast<const char*>(arg1);
    uint64_t userland_proc_fl = arg2;
    uint32_t access_rights = static_cast<uint32_t>(arg3);
    uint32_t handle_flags = static_cast<uint32_t>(arg4);
    struct userland_proc_info {
        int32_t pid;        // Process ID
        char name[256];     // Process name
        int inherit_pty;    // Indicator whether the specified pty should be inherited
        int pty_handle;     // Handle to the PTY device in the calling process to set as the target process's PTY
    } *info = reinterpret_cast<struct userland_proc_info*>(arg5);

    if (!path) {
        return -EINVAL;
    }

    // Load the ELF file and create a process core
    process_core* core = elf::elf64_loader::load_from_file(path);
    if (!core) {
        return -ENOMEM;
    }

    // Create a new process with the loaded core
    process* new_proc = new process();
    if (!new_proc) {
        sched::destroy_process_core(core);
        return -ENOMEM;
    }

    process_creation_flags new_proc_flags = process_creation_flags::SCHEDULE_NOW;

    if (userland_proc_fl & PROC_CAN_ELEVATE) {
        new_proc_flags |= process_creation_flags::CAN_ELEVATE;
    }

    // Handle PTY inheritance
    pty* pty_device = nullptr;
    if (info && info->inherit_pty == 1 && info->pty_handle >= 0) {
        handle_entry* pty_handle_entry = current->get_env()->handles.get_handle(info->pty_handle);
        if (pty_handle_entry && pty_handle_entry->type == handle_type::PTY_DEVICE) {
            pty_device = reinterpret_cast<pty*>(pty_handle_entry->object);
        }
    }

    process_env* env = new process_env(pty_device);
    env->creation_flags = new_proc_flags;

    // Initialize the process with the loaded core
    if (!new_proc->init(core, true, env, true)) {
        new_proc->cleanup(); // Will call `destroy_process_core`
        delete new_proc;
        return -ENOMEM;
    }

    // Add a handle to the new process in the parent's environment
    size_t handle_index = current->get_env()->handles.add_handle(
        handle_type::PROCESS,
        new_proc,
        access_rights,
        handle_flags,
        static_cast<uint64_t>(core->identity.pid) // Store PID in metadata
    );

    if (handle_index == SIZE_MAX) {
        // If we can't add the handle, we need to clean up the process
        new_proc->cleanup();
        delete new_proc;
        return -ENOMEM;
    }

    // Increment the reference count for the parent's handle
    new_proc->add_ref();

    // Fill in process info if requested
    if (info) {
        info->pid = core->identity.pid;
        memcpy(info->name, core->identity.name, sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
    }

    // Return the handle index
    return static_cast<long>(handle_index);
}

DECLARE_SYSCALL_HANDLER(proc_wait) {
    // arg1 = handle to wait for
    // arg2 = pointer to store exit code
    int32_t handle = static_cast<int32_t>(arg1);
    int* exit_code = reinterpret_cast<int*>(arg2);

    if (handle < 0) {
        return -EINVAL;
    }

    // Get the handle entry
    handle_entry* hentry = current->get_env()->handles.get_handle(handle);
    if (!hentry || hentry->type != handle_type::PROCESS) {
        return -EINVAL;  // Invalid handle
    }

    // Get the process pointer from the handle
    process* target_proc = reinterpret_cast<process*>(hentry->object);
    if (!target_proc) {
        return -EINVAL;  // Invalid handle
    }

    // Wait for the process to terminate
    while (target_proc->get_core()->state != process_state::TERMINATED) {
        // Yield to other processes while waiting
        sched::yield();
    }

    // Get the exit code if requested
    if (exit_code) {
        // TODO: Implement proper exit code tracking
        *exit_code = 0;
    }

    // Remove the handle and release our reference
    current->get_env()->handles.remove_handle(handle);
    target_proc->release_ref();

    // Add the process to the cleanup queue if needed
    if (target_proc->get_core()->ctx_switch_state.needs_cleanup == 1) {
        sched::scheduler::get().add_to_cleanup_queue(target_proc);
    }

    return 0;
}

DECLARE_SYSCALL_HANDLER(proc_close) {
    // arg1 = handle to close
    int32_t handle = static_cast<int32_t>(arg1);

    if (handle < 0) {
        return -EINVAL;
    }

    // Get the handle entry
    handle_entry* hentry = current->get_env()->handles.get_handle(handle);
    if (!hentry) {
        return -EINVAL;  // Invalid handle
    }

    // Get the object pointer
    void* object = hentry->object;
    if (!object) {
        return -EINVAL;  // Invalid handle
    }

    // Handle type-specific cleanup
    switch (hentry->type) {
    case handle_type::PROCESS: {
        process* proc = reinterpret_cast<process*>(object);
        proc->release_ref();

        // Add the process to the cleanup queue if needed
        if (proc->get_core()->ctx_switch_state.needs_cleanup == 1) {
            sched::scheduler::get().add_to_cleanup_queue(proc);
        }
        break;
    }
    // Add more handle type cleanup
    default:
        break;
    }

    // Remove the handle
    if (!current->get_env()->handles.remove_handle(handle)) {
        return -EINVAL;
    }

    return 0;
} 