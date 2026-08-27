#include "syscall/handlers/sys_futex.h"
#include "sync/futex.h"
#include "mm/uaccess.h"

// Relative timeout as userland passes it to the futex syscall
struct futex_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

// The private flag is meaningless because waiters are already keyed
// by address space, and requeue degrades to waking every waiter
constexpr uint64_t FUTEX_CMD_MASK       = 0x7F;
constexpr uint64_t FUTEX_OP_WAIT        = 0;
constexpr uint64_t FUTEX_OP_WAKE        = 1;
constexpr uint64_t FUTEX_OP_REQUEUE     = 3;
constexpr uint64_t FUTEX_OP_CMP_REQUEUE = 4;

constexpr int64_t NSEC_PER_SEC = 1000000000;

// Zero nanoseconds means wait forever in the native layer, so an already
// expired timeout clamps to one nanosecond. Returns 0 or a negative errno
static int64_t read_futex_timeout(uint64_t u_timeout, uint64_t* out_ns) {
    if (u_timeout == 0) {
        *out_ns = 0;
        return 0;
    }

    futex_timespec ts;

    if (mm::uaccess::copy_from_user(
            &ts, reinterpret_cast<const void*>(u_timeout),
            sizeof(ts)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= NSEC_PER_SEC) {
        return syscall::EINVAL;
    }

    uint64_t ns = static_cast<uint64_t>(ts.tv_sec) * NSEC_PER_SEC
                + static_cast<uint64_t>(ts.tv_nsec);

    *out_ns = (ns == 0) ? 1 : ns;

    return 0;
}

DEFINE_SYSCALL6(futex, u_uaddr, u_op, u_val, u_timeout, u_uaddr2, u_val3) {
    (void)u_uaddr2;

    uint64_t cmd = u_op & FUTEX_CMD_MASK;
    uintptr_t uaddr = static_cast<uintptr_t>(u_uaddr);

    switch (cmd) {
    case FUTEX_OP_WAIT: {
        uint64_t timeout_ns = 0;

        int64_t rc = read_futex_timeout(u_timeout, &timeout_ns);
        if (rc != 0) {
            return rc;
        }

        // EINTR is returned as is, musl retries interrupted waits itself
        return sync::futex_wait(
            uaddr, static_cast<uint32_t>(u_val), timeout_ns);
    }

    case FUTEX_OP_WAKE:
        return sync::futex_wake(uaddr, static_cast<uint32_t>(u_val));

    case FUTEX_OP_CMP_REQUEUE: {
        uint32_t cur = 0;

        if (mm::uaccess::copy_from_user(
                &cur, reinterpret_cast<const void*>(u_uaddr),
                sizeof(cur)) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }

        if (cur != static_cast<uint32_t>(u_val3)) {
            return syscall::EAGAIN;
        }

        return sync::futex_wake_all(uaddr);
    }

    case FUTEX_OP_REQUEUE:
        return sync::futex_wake_all(uaddr);

    default:
        return syscall::ENOSYS;
    }
}

DEFINE_SYSCALL3(futex_wait, uaddr, expected, timeout_ns) {
    int64_t rc = sync::futex_wait(
        static_cast<uintptr_t>(uaddr),
        static_cast<uint32_t>(expected),
        timeout_ns);

    // An interrupted wait restarts safely, re-checking the futex value
    return rc == syscall::EINTR ? syscall::ERESTARTSYS : rc;
}

DEFINE_SYSCALL2(futex_wake, uaddr, count) {
    return sync::futex_wake(
        static_cast<uintptr_t>(uaddr),
        static_cast<uint32_t>(count));
}

DEFINE_SYSCALL1(futex_wake_all, uaddr) {
    return sync::futex_wake_all(static_cast<uintptr_t>(uaddr));
}
