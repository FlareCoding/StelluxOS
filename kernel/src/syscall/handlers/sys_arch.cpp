#include <syscall/handlers/sys_arch.h>
#include <arch/x86/msr.h>
#include <dynpriv/dynpriv.h>
#include <process/process.h>
#include <core/klog.h>
#include <acpi/fadt.h>

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

DECLARE_SYSCALL_HANDLER(set_thread_area) {
    uint64_t code = arg1;
    uint64_t tls_userptr = arg2;
    uint64_t tls_size = arg3;
    __unused tls_size;

    long return_val = 0;

    switch (code) {
    case ARCH_SET_FS: {
        SYSCALL_TRACE("arch_prctl(ARCH_SET_FS, 0x%llx) = 0\n", tls_userptr);
        current->get_core()->fs_base = tls_userptr;
        arch::x86::msr::write(IA32_FS_BASE, tls_userptr);
        break;
    }
    case ARCH_GET_FS: {
        SYSCALL_TRACE("arch_prctl(ARCH_GET_FS, 0x%llx) = 0 [writing 0x%llx]\n", tls_userptr, current->get_core()->fs_base);
        *reinterpret_cast<uint64_t*>(tls_userptr) = current->get_core()->fs_base;
        break;
    }
    default: {
        return_val = -EINVAL;
        break;
    }
    }

    return return_val;
}

DECLARE_SYSCALL_HANDLER(set_tid_address) {
    SYSCALL_TRACE("set_tid_address(0x%llx) = %d\n", arg1, current->get_core()->identity.pid);
    return static_cast<long>(current->get_core()->identity.pid);
}

DECLARE_SYSCALL_HANDLER(reboot) {
    uint64_t magic1 = arg1;
    uint64_t magic2 = arg2;
    uint64_t cmd = arg3; __unused cmd;
    uint64_t arg = arg4; __unused arg;
    
    // Linux reboot syscall magic numbers
    const uint64_t LINUX_REBOOT_MAGIC1 = 0xfee1dead;
    const uint64_t LINUX_REBOOT_MAGIC2 = 0x28121969;
    const uint64_t LINUX_REBOOT_MAGIC2A = 0x05121996;
    const uint64_t LINUX_REBOOT_MAGIC2B = 0x16041998;
    const uint64_t LINUX_REBOOT_MAGIC2C = 0x20112000;
    
    // Check magic numbers
    if (magic1 != LINUX_REBOOT_MAGIC1) {
        SYSCALL_TRACE("reboot(0x%llx, 0x%llx, %llu, %llu) = -EINVAL (invalid magic1)\n", 
                      magic1, magic2, cmd, arg);
        return -EINVAL;
    }
    
    if (magic2 != LINUX_REBOOT_MAGIC2 && 
        magic2 != LINUX_REBOOT_MAGIC2A && 
        magic2 != LINUX_REBOOT_MAGIC2B && 
        magic2 != LINUX_REBOOT_MAGIC2C) {
        SYSCALL_TRACE("reboot(0x%llx, 0x%llx, %llu, %llu) = -EINVAL (invalid magic2)\n", 
                      magic1, magic2, cmd, arg);
        return -EINVAL;
    }
    
    SYSCALL_TRACE("reboot(0x%llx, 0x%llx, %llu, %llu) = 0\n", magic1, magic2, cmd, arg);
    
    // For now, ust reboot regardless of the command
    // TO-DO: we'd handle different reboot commands
    kprint("[*] System reboot requested via syscall\n");
    
    // Trigger ACPI reboot
    acpi::fadt::get().reboot();
    
    // This should never return, but just in case
    return 0;
}

DECLARE_SYSCALL_HANDLER(elevate) {
    int original_elevate_status = static_cast<int>(arg1);

    // Make sure that the thread is allowed to elevate
    if (!dynpriv::is_asid_allowed()) {
        kprint("[*] Unauthorized elevation attempt\n");
        return -ENOPRIV;
    }

    if (original_elevate_status) {
        kprint("[*] Already elevated\n");
    } else {
        current->get_core()->hw_state.elevated = 1;
    }

    return 0;
}
