#ifndef SYSCALLS_H
#define SYSCALLS_H
#include <ktypes.h>

#define ENOSYS static_cast<long>(-1)

#define SYSCALL_SYS_WRITE   0
#define SYSCALL_SYS_READ    1
#define SYSCALL_SYS_EXIT    60

#endif
