#include "syscall/syscall_table.h"
#include "syscall/linux_syscalls.h"
#include "syscall/handlers/sys_arch.h"

__PRIVILEGED_CODE void syscall::register_arch_syscalls() {
    REGISTER_SYSCALL(linux_nr::ARCH_PRCTL, arch_prctl);
}
