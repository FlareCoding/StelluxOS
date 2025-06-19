#include <stlibc/proc/proc.h>

int proc_create(const char* path, uint64_t flags, uint32_t access_rights, uint32_t handle_flags, struct proc_info* info) {
    // // Create process and get handle
    // int handle = static_cast<int>(
    //     syscall(SYS_PROC_CREATE, 
    //         reinterpret_cast<uint64_t>(path), 
    //         flags,
    //         access_rights,
    //         handle_flags,
    //         reinterpret_cast<uint64_t>(info)
    //     )
    // );
    //
    // return handle;

    return -1;
}

int proc_wait(int handle, int* exit_code) {
    // long result = syscall(SYS_PROC_WAIT, handle, reinterpret_cast<uint64_t>(exit_code), 0, 0, 0);
    // return static_cast<int>(result);

    return -1;
}

int proc_close(int handle) {
    // long result = syscall(SYS_PROC_CLOSE, handle, 0, 0, 0, 0);
    // return static_cast<int>(result);

    return -1;
}