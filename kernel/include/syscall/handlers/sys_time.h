#ifndef SYS_TIME_H
#define SYS_TIME_H

#include <syscall/syscall_registry.h>

// Declare all time-related syscall handlers
DECLARE_SYSCALL_HANDLER(nanosleep);
DECLARE_SYSCALL_HANDLER(clock_gettime);

#endif // SYS_TIME_H
