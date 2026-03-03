#include "syscall/handlers/sys_proc.h"

DEFINE_SYSCALL2(proc_create, path_ptr, argv_ptr) {
    return syscall::ENOSYS;
}

DEFINE_SYSCALL1(proc_start, handle) {
    return syscall::ENOSYS;
}

DEFINE_SYSCALL2(proc_wait, handle, exit_code_ptr) {
    return syscall::ENOSYS;
}

DEFINE_SYSCALL1(proc_detach, handle) {
    return syscall::ENOSYS;
}

DEFINE_SYSCALL2(proc_info, handle, info_ptr) {
    return syscall::ENOSYS;
}
