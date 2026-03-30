#include "syscall/handlers/sys_signal.h"

// No-op stub: Stellux does not deliver signals yet
DEFINE_SYSCALL4(rt_sigaction, signum, u_act, u_oldact, sigsetsize) {
    (void)signum;
    (void)u_act;
    (void)u_oldact;
    (void)sigsetsize;
    return 0;
}

// No-op stub: signal mask manipulation has no effect without signal delivery
DEFINE_SYSCALL4(rt_sigprocmask, how, u_set, u_oldset, sigsetsize) {
    (void)how;
    (void)u_set;
    (void)u_oldset;
    (void)sigsetsize;
    return 0;
}
