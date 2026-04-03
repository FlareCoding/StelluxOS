#include "syscall/handlers/sys_futex.h"
#include "sync/futex.h"

DEFINE_SYSCALL3(futex_wait, uaddr, expected, timeout_ns) {
    return sync::futex_wait(
        static_cast<uintptr_t>(uaddr),
        static_cast<uint32_t>(expected),
        timeout_ns);
}

DEFINE_SYSCALL2(futex_wake, uaddr, count) {
    return sync::futex_wake(
        static_cast<uintptr_t>(uaddr),
        static_cast<uint32_t>(count));
}

DEFINE_SYSCALL1(futex_wake_all, uaddr) {
    return sync::futex_wake_all(static_cast<uintptr_t>(uaddr));
}
