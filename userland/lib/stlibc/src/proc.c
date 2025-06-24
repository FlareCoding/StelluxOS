#include <stlibc/proc/proc.h>
#include <stlibc/stellux_syscalls.h>

int stlx_proc_create(const char* path, uint64_t flags, uint32_t access_rights, uint32_t handle_flags, struct proc_info* info) {
    // Create process and get handle
    int handle = (int)syscall5(
        SYS_PROC_CREATE, 
        (uint64_t)path, 
        flags,
        access_rights,
        handle_flags,
        (uint64_t)info
    );
    
    return handle;
}

int stlx_proc_wait(int handle, int* exit_code) {
    long result = syscall2(SYS_PROC_WAIT, handle, (uint64_t)exit_code);
    return (int)result;
}

int stlx_proc_close(int handle) {
    long result = syscall1(SYS_PROC_CLOSE, handle);
    return (int)result;
}