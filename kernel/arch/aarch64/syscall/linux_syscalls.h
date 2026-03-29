#ifndef STELLUX_SYSCALL_LINUX_SYSCALLS_H
#define STELLUX_SYSCALL_LINUX_SYSCALLS_H

#include "common/types.h"

namespace syscall::linux_nr {

constexpr uint64_t GETCWD           = 17;
constexpr uint64_t FCNTL            = 25;
constexpr uint64_t IOCTL            = 29;
constexpr uint64_t MKDIRAT          = 34;
constexpr uint64_t UNLINKAT         = 35;
constexpr uint64_t LSEEK            = 62;
constexpr uint64_t FTRUNCATE        = 46;
constexpr uint64_t CHDIR            = 49;
constexpr uint64_t FCHDIR           = 50;
constexpr uint64_t OPENAT           = 56;
constexpr uint64_t CLOSE            = 57;
constexpr uint64_t GETDENTS64       = 61;
constexpr uint64_t READ             = 63;
constexpr uint64_t WRITE            = 64;
constexpr uint64_t WRITEV           = 66;
constexpr uint64_t NEWFSTATAT       = 79;
constexpr uint64_t FSTAT            = 80;
constexpr uint64_t EXIT             = 93;
constexpr uint64_t EXIT_GROUP       = 94;
constexpr uint64_t SET_TID_ADDRESS  = 96;
constexpr uint64_t NANOSLEEP        = 101;
constexpr uint64_t CLOCK_GETTIME    = 113;
constexpr uint64_t CLOCK_GETRES     = 114;
constexpr uint64_t GETTIMEOFDAY     = 169;
constexpr uint64_t GETPID           = 172;
constexpr uint64_t SOCKET           = 198;
constexpr uint64_t SOCKETPAIR       = 199;
constexpr uint64_t BIND             = 200;
constexpr uint64_t LISTEN           = 201;
constexpr uint64_t ACCEPT           = 202;
constexpr uint64_t CONNECT          = 203;
constexpr uint64_t SENDTO           = 206;
constexpr uint64_t RECVFROM         = 207;
constexpr uint64_t SETSOCKOPT       = 208;
constexpr uint64_t GETSOCKOPT       = 209;
constexpr uint64_t BRK              = 214;
constexpr uint64_t MUNMAP           = 215;
constexpr uint64_t MMAP             = 222;
constexpr uint64_t MPROTECT         = 226;
constexpr uint64_t GETRANDOM        = 278;
constexpr uint64_t MEMFD_CREATE     = 279;

} // namespace syscall::linux_nr

#endif // STELLUX_SYSCALL_LINUX_SYSCALLS_H
