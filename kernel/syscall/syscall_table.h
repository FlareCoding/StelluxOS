#ifndef STELLUX_SYSCALL_SYSCALL_TABLE_H
#define STELLUX_SYSCALL_SYSCALL_TABLE_H

#include "common/types.h"

namespace syscall {

using handler_t = int64_t (*)(uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t);

constexpr uint64_t MAX_SYSCALL_NUM = 2048;
constexpr int64_t EPERM  = -1;
constexpr int64_t ENOENT = -2;
constexpr int64_t ESRCH  = -3;
constexpr int64_t EINTR  = -4;
constexpr int64_t EIO    = -5;
constexpr int64_t EBADF  = -9;
constexpr int64_t ENOMEM = -12;
constexpr int64_t EACCES = -13;
constexpr int64_t EFAULT = -14;
constexpr int64_t EEXIST  = -17;
constexpr int64_t ENOTDIR = -20;
constexpr int64_t EISDIR  = -21;
constexpr int64_t EINVAL  = -22;
constexpr int64_t EMFILE  = -24;
constexpr int64_t ENOTTY = -25;
constexpr int64_t ERANGE = -34;
constexpr int64_t ENAMETOOLONG = -36;
constexpr int64_t ENOSYS    = -38;
constexpr int64_t ENOTEMPTY = -39;
constexpr int64_t ELOOP     = -40;
constexpr int64_t EAGAIN    = -11;
constexpr int64_t EBUSY     = -16;
constexpr int64_t ESPIPE = -29;
constexpr int64_t EPIPE  = -32;
constexpr int64_t ENOTSOCK = -88;
constexpr int64_t EMSGSIZE         = -90;
constexpr int64_t ENOPROTOOPT      = -92;
constexpr int64_t EPROTONOSUPPORT  = -93;
constexpr int64_t EOPNOTSUPP       = -95;
constexpr int64_t EAFNOSUPPORT     = -97;
constexpr int64_t EADDRINUSE       = -98;
constexpr int64_t EISCONN          = -106;
constexpr int64_t ENOTCONN         = -107;
constexpr int64_t ECONNREFUSED     = -111;

extern handler_t g_syscall_table[MAX_SYSCALL_NUM];

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_syscall_table();

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void register_arch_syscalls();

} // namespace syscall

#define DECLARE_SYSCALL(name) \
    __PRIVILEGED_CODE int64_t sys_##name( \
        uint64_t, uint64_t, uint64_t, \
        uint64_t, uint64_t, uint64_t)

#define DEFINE_SYSCALL0(name) \
    __PRIVILEGED_CODE int64_t sys_##name( \
        uint64_t, uint64_t, uint64_t, \
        uint64_t, uint64_t, uint64_t)

#define DEFINE_SYSCALL1(name, p1) \
    __PRIVILEGED_CODE int64_t sys_##name( \
        uint64_t p1, \
        uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)

#define DEFINE_SYSCALL2(name, p1, p2) \
    __PRIVILEGED_CODE int64_t sys_##name( \
        uint64_t p1, uint64_t p2, \
        uint64_t, uint64_t, uint64_t, uint64_t)

#define DEFINE_SYSCALL3(name, p1, p2, p3) \
    __PRIVILEGED_CODE int64_t sys_##name( \
        uint64_t p1, uint64_t p2, uint64_t p3, \
        uint64_t, uint64_t, uint64_t)

#define DEFINE_SYSCALL4(name, p1, p2, p3, p4) \
    __PRIVILEGED_CODE int64_t sys_##name( \
        uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, \
        uint64_t, uint64_t)

#define DEFINE_SYSCALL5(name, p1, p2, p3, p4, p5) \
    __PRIVILEGED_CODE int64_t sys_##name( \
        uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5, \
        uint64_t)

#define DEFINE_SYSCALL6(name, p1, p2, p3, p4, p5, p6) \
    __PRIVILEGED_CODE int64_t sys_##name( \
        uint64_t p1, uint64_t p2, uint64_t p3, \
        uint64_t p4, uint64_t p5, uint64_t p6)

#define REGISTER_SYSCALL(num, name) \
    syscall::g_syscall_table[num] = sys_##name

#endif // STELLUX_SYSCALL_SYSCALL_TABLE_H
