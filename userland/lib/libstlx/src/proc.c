#define _GNU_SOURCE
#include <stlx/proc.h>
#include <stlx/syscall_nums.h>
#include <unistd.h>

int proc_create(const char* path, const char* argv[]) {
    return (int)syscall(SYS_PROC_CREATE, path, argv);
}

int proc_exec(const char* path, const char* argv[]) {
    int handle = proc_create(path, argv);
    if (handle < 0) {
        return handle;
    }
    int err = proc_start(handle);
    if (err < 0) {
        close(handle);
        return err;
    }
    return handle;
}

int proc_start(int handle) {
    return (int)syscall(SYS_PROC_START, handle);
}

int proc_wait(int handle, int* exit_code) {
    return (int)syscall(SYS_PROC_WAIT, handle, exit_code);
}

int proc_detach(int handle) {
    return (int)syscall(SYS_PROC_DETACH, handle);
}

int proc_info(int handle, process_info* info) {
    return (int)syscall(SYS_PROC_INFO, handle, info);
}

int proc_set_handle(int proc_handle, int slot, int resource_handle) {
    return (int)syscall(SYS_PROC_SET_HANDLE, proc_handle, slot, resource_handle);
}

int proc_kill(int handle) {
    return (int)syscall(SYS_PROC_KILL, handle);
}
