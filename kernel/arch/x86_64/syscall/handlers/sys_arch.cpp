#include "syscall/handlers/sys_arch.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"
#include "hw/cpu.h"
#include "mm/uaccess.h"
#include "syscall/syscall_table.h"

constexpr uint64_t ARCH_SET_FS  = 0x1002;
constexpr uint64_t ARCH_GET_FS  = 0x1003;

DEFINE_SYSCALL2(arch_prctl, code, addr) {
    sched::task_exec_core* task = this_cpu(current_task_exec);
    switch (code) {
        case ARCH_SET_FS:
            task->tls_base = addr;
            cpu::write_tls_base(addr);
            return 0;
        case ARCH_GET_FS: {
            int32_t rc = mm::uaccess::copy_to_user(
                reinterpret_cast<void*>(addr), &task->tls_base, sizeof(task->tls_base)
            );

            if (rc != mm::uaccess::OK) {
                return syscall::EFAULT;
            }

            return 0;
        }
        default:
            return syscall::EINVAL;
    }
}
