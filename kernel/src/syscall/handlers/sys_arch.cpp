#include <syscall/handlers/sys_arch.h>
#include <arch/x86/msr.h>
#include <dynpriv/dynpriv.h>
#include <process/process.h>
#include <core/klog.h>

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