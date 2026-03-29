#ifndef STELLUX_SYSCALL_LINUX_SYSCALLS_H
#define STELLUX_SYSCALL_LINUX_SYSCALLS_H

#include "common/types.h"

namespace syscall::linux_nr {

constexpr uint64_t READ             = 0;
constexpr uint64_t WRITE            = 1;
constexpr uint64_t OPEN             = 2;
constexpr uint64_t CLOSE            = 3;
constexpr uint64_t STAT             = 4;
constexpr uint64_t LSEEK            = 8;
constexpr uint64_t FSTAT            = 5;
constexpr uint64_t LSTAT            = 6;
constexpr uint64_t POLL             = 7;
constexpr uint64_t MMAP             = 9;
constexpr uint64_t BRK              = 12;
constexpr uint64_t MPROTECT         = 10;
constexpr uint64_t MUNMAP           = 11;
constexpr uint64_t IOCTL            = 16;
constexpr uint64_t WRITEV           = 20;
constexpr uint64_t NANOSLEEP        = 35;
constexpr uint64_t GETPID           = 39;
constexpr uint64_t SOCKET           = 41;
constexpr uint64_t CONNECT          = 42;
constexpr uint64_t ACCEPT           = 43;
constexpr uint64_t SENDTO           = 44;
constexpr uint64_t RECVFROM         = 45;
constexpr uint64_t SHUTDOWN         = 48;
constexpr uint64_t BIND             = 49;
constexpr uint64_t LISTEN           = 50;
constexpr uint64_t SOCKETPAIR       = 53;
constexpr uint64_t SETSOCKOPT       = 54;
constexpr uint64_t GETSOCKOPT       = 55;
constexpr uint64_t EXIT             = 60;
constexpr uint64_t FCNTL            = 72;
constexpr uint64_t FTRUNCATE        = 77;
constexpr uint64_t GETCWD           = 79;
constexpr uint64_t CHDIR            = 80;
constexpr uint64_t FCHDIR           = 81;
constexpr uint64_t MKDIR            = 83;
constexpr uint64_t RMDIR            = 84;
constexpr uint64_t UNLINK           = 87;
constexpr uint64_t GETTIMEOFDAY     = 96;
constexpr uint64_t ARCH_PRCTL       = 158;
constexpr uint64_t GETDENTS64       = 217;
constexpr uint64_t SET_TID_ADDRESS  = 218;
constexpr uint64_t CLOCK_GETTIME    = 228;
constexpr uint64_t CLOCK_GETRES     = 229;
constexpr uint64_t EXIT_GROUP       = 231;
constexpr uint64_t OPENAT           = 257;
constexpr uint64_t NEWFSTATAT       = 262;
constexpr uint64_t MKDIRAT          = 258;
constexpr uint64_t UNLINKAT         = 263;
constexpr uint64_t PPOLL            = 271;
constexpr uint64_t GETRANDOM        = 318;
constexpr uint64_t MEMFD_CREATE     = 319;

} // namespace syscall::linux_nr

#endif // STELLUX_SYSCALL_LINUX_SYSCALLS_H
