#include "syscall/handlers/sys_task.h"
#include "sched/sched.h"
#include "sched/task.h"

DEFINE_SYSCALL0(set_tid_address) {
    return static_cast<int64_t>(sched::current()->tid);
}

DEFINE_SYSCALL1(exit, status) {
    sched::exit(static_cast<int>(status));
    __builtin_unreachable();
}

DEFINE_SYSCALL1(exit_group, status) {
    sched::exit(static_cast<int>(status));
    __builtin_unreachable();
}
