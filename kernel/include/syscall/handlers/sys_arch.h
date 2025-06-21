#ifndef SYS_ARCH_H
#define SYS_ARCH_H

#include <syscall/syscall_registry.h>

// Declare all architecture-specific syscall handlers
DECLARE_SYSCALL_HANDLER(set_thread_area);
DECLARE_SYSCALL_HANDLER(set_tid_address);

DECLARE_SYSCALL_HANDLER(elevate);

#endif // SYS_ARCH_H 