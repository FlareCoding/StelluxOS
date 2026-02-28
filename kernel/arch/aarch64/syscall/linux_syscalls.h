#ifndef STELLUX_SYSCALL_LINUX_SYSCALLS_H
#define STELLUX_SYSCALL_LINUX_SYSCALLS_H

#include "common/types.h"

namespace syscall::linux_nr {

constexpr uint64_t EXIT             = 93;
constexpr uint64_t EXIT_GROUP       = 94;
constexpr uint64_t SET_TID_ADDRESS  = 96;

} // namespace syscall::linux_nr

#endif // STELLUX_SYSCALL_LINUX_SYSCALLS_H
