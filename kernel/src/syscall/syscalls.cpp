#include "syscalls.h"
#include <process/process.h>
#include <sched/sched.h>
#include <arch/x86/per_cpu_data.h>
#include <kprint.h>

EXTERN_C long __check_current_elevate_status() {
    return static_cast<long>(current->elevated);
}

EXTERN_C long __syscall_handler(
    uint64_t syscallnum,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5
) {
    uint64_t returnVal = 0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    switch (syscallnum) {
    case SYSCALL_SYS_WRITE: {
        // Handle write syscall
        kprint((const char*)arg2);
        break;
    }
    case SYSCALL_SYS_READ: {
        // Handle read syscall
        break;
    }
    case SYSCALL_SYS_EXIT: {
        auto& sched = Scheduler::get();

        // Get the current task
        PCB* currentTask = sched.getCurrentTask();
        PCB* nextTask = sched.getNextTask();

        kprint("Exiting userspace process with pid:%i...\n", currentTask->pid);

        // Remove the current task from the scheduler task queue
        sched.removeTask(currentTask->pid);

        // Switch to the next task
        switchTo(currentTask, nextTask);

        // ------------------------------------------------ //
        // This part should never be reached since switchTo //
        // should take an iretq path to the next process.   //
        // ------------------------------------------------ //
        break;
    }
    case SYSCALL_SYS_ELEVATE: {
        if (current->elevated) {
            kprint("[*] Already elevated\n");
        } else {
            current->elevated = 1;
            //__per_cpu_data.__cpu[0].elevated = 1;
        }
        break;
    }
    case SYSCALL_SYS_LOWER: {
        if (!current->elevated) {
            kprint("[*] Already lowered\n");
        } else {
            current->elevated = 0;
            //__per_cpu_data.__cpu[0].elevated = 0;
        }
        break;
    }
    default: {
        kprintError("Unknown syscall number %llu\n", syscallnum);
        returnVal = -ENOSYS;
        break;
    }
    }

    return returnVal;
}
