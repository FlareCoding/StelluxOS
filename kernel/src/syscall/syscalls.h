#ifndef SYSCALLS_H
#define SYSCALLS_H
#include <ktypes.h>

#define ENOSYS 1

#define SYSCALL_SYS_WRITE       0
#define SYSCALL_SYS_READ        1
#define SYSCALL_SYS_EXIT        60

#define SYSCALL_SYS_ELEVATE     91
#define SYSCALL_SYS_LOWER       92

#endif
