#ifndef STELLUX_SYSCALL_HANDLERS_SYS_PROC_H
#define STELLUX_SYSCALL_HANDLERS_SYS_PROC_H

#include "syscall/syscall_table.h"

DECLARE_SYSCALL(proc_create);
DECLARE_SYSCALL(proc_start);
DECLARE_SYSCALL(proc_wait);
DECLARE_SYSCALL(proc_detach);
DECLARE_SYSCALL(proc_info);
DECLARE_SYSCALL(proc_set_handle);
DECLARE_SYSCALL(proc_kill);
DECLARE_SYSCALL(proc_create_thread);

#endif // STELLUX_SYSCALL_HANDLERS_SYS_PROC_H
