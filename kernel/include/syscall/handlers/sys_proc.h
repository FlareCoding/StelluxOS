#ifndef SYS_PROC_H
#define SYS_PROC_H

#include <syscall/syscall_registry.h>

// Declare all process management syscall handlers
DECLARE_SYSCALL_HANDLER(getpid);
DECLARE_SYSCALL_HANDLER(exit);
DECLARE_SYSCALL_HANDLER(exit_group);
DECLARE_SYSCALL_HANDLER(proc_create);
DECLARE_SYSCALL_HANDLER(proc_wait);
DECLARE_SYSCALL_HANDLER(proc_close);

#endif // SYS_PROC_H 