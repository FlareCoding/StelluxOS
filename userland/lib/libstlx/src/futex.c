#define _GNU_SOURCE
#include <stlx/futex.h>
#include <stlx/syscall_nums.h>
#include <unistd.h>

int stlx_futex_wait(uint32_t* addr, uint32_t expected, uint64_t timeout_ns) {
    return (int)syscall(SYS_FUTEX_WAIT, addr, expected, timeout_ns);
}

int stlx_futex_wake(uint32_t* addr, uint32_t count) {
    return (int)syscall(SYS_FUTEX_WAKE, addr, count);
}

int stlx_futex_wake_all(uint32_t* addr) {
    return (int)syscall(SYS_FUTEX_WAKE_ALL, addr);
}
