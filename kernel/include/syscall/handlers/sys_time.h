#ifndef SYS_TIME_H
#define SYS_TIME_H

#include <syscall/syscall_registry.h>

// Declare all time-related syscall handlers
DECLARE_SYSCALL_HANDLER(nanosleep);

#endif // SYS_TIME_H
