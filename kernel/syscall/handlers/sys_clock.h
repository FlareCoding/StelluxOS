#ifndef STELLUX_SYSCALL_HANDLERS_SYS_CLOCK_H
#define STELLUX_SYSCALL_HANDLERS_SYS_CLOCK_H

#include "syscall/syscall_table.h"

DECLARE_SYSCALL(clock_gettime);
DECLARE_SYSCALL(clock_getres);
DECLARE_SYSCALL(clock_nanosleep);
DECLARE_SYSCALL(nanosleep);
DECLARE_SYSCALL(gettimeofday);

#endif // STELLUX_SYSCALL_HANDLERS_SYS_CLOCK_H
