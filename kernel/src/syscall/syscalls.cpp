#include <syscall/syscalls.h>
#include <syscall/syscall_registry.h>
#include <syscall/handlers/sys_io.h>
#include <syscall/handlers/sys_mem.h>
#include <syscall/handlers/sys_proc.h>
#include <syscall/handlers/sys_arch.h>
#include <syscall/handlers/sys_graphics.h>
#include <syscall/handlers/sys_time.h>
#include <syscall/handlers/sys_net.h>
#include <process/process.h>
#include <core/klog.h>

// The global syscall table - initialized at runtime
syscall_handler_t g_syscall_table[MAX_SYSCALL_NUM];

// Helper macro to register a syscall at runtime
#define REGISTER_SYSCALL(num, name) \
    g_syscall_table[num] = sys_##name##_handler

// Initialize the syscall table
__PRIVILEGED_CODE
void init_syscall_table() {
    // Initialize all entries to nullptr
    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        g_syscall_table[i] = nullptr;
    }
    
    // Register syscalls using the same macro pattern
    REGISTER_SYSCALL(SYSCALL_SYS_READ, read);
    REGISTER_SYSCALL(SYSCALL_SYS_WRITE, write);
    REGISTER_SYSCALL(SYSCALL_SYS_OPEN, open);
    REGISTER_SYSCALL(SYSCALL_SYS_CLOSE, close);
    REGISTER_SYSCALL(SYSCALL_SYS_LSEEK, lseek);
    REGISTER_SYSCALL(SYSCALL_SYS_MMAP, mmap);
    REGISTER_SYSCALL(SYSCALL_SYS_MUNMAP, munmap);
    REGISTER_SYSCALL(SYSCALL_SYS_BRK, brk);
    REGISTER_SYSCALL(SYSCALL_SYS_IOCTL, ioctl);
    REGISTER_SYSCALL(SYSCALL_SYS_WRITEV, writev);
    REGISTER_SYSCALL(SYSCALL_SYS_NANOSLEEP, nanosleep);
    REGISTER_SYSCALL(SYSCALL_SYS_GETPID, getpid);
    REGISTER_SYSCALL(SYSCALL_SYS_SOCKET, socket);
    REGISTER_SYSCALL(SYSCALL_SYS_CONNECT, connect);
    REGISTER_SYSCALL(SYSCALL_SYS_ACCEPT, accept);
    REGISTER_SYSCALL(SYSCALL_SYS_BIND, bind);
    REGISTER_SYSCALL(SYSCALL_SYS_LISTEN, listen);
    REGISTER_SYSCALL(SYSCALL_SYS_EXIT, exit);
    REGISTER_SYSCALL(SYSCALL_SYS_SET_THREAD_AREA, set_thread_area);
    REGISTER_SYSCALL(SYSCALL_SYS_SET_TID_ADDRESS, set_tid_address);
    REGISTER_SYSCALL(SYSCALL_SYS_EXIT_GROUP, exit_group);
    
    REGISTER_SYSCALL(SYSCALL_SYS_PROC_CREATE, proc_create);
    REGISTER_SYSCALL(SYSCALL_SYS_PROC_WAIT, proc_wait);
    REGISTER_SYSCALL(SYSCALL_SYS_PROC_CLOSE, proc_close);
    
    REGISTER_SYSCALL(SYSCALL_SYS_ELEVATE, elevate);
    
    REGISTER_SYSCALL(SYSCALL_SYS_GRAPHICS_FRAMEBUFFER_OP, gfx_fb_op);
}

EXTERN_C
__PRIVILEGED_CODE
long __syscall_handler(
    uint64_t syscallnum,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
) {
    // Make sure the elevation status patch happens uninterrupted
    disable_interrupts();

    // After faking out being elevated, original elevation
    // privileged should be restored, except in the case
    // when the scheduler switched context into a new task.
    int original_elevate_status = current->get_core()->hw_state.elevated;
    process* original_task = current;

    current->get_core()->hw_state.elevated = 1;

    // Re-enable interrupts after the elevated status is patched
    enable_interrupts();

    // If this is an ELEVATE syscall, set arg1 to be the original elevate status
    if (syscallnum == SYSCALL_SYS_ELEVATE) {
        arg1 = original_elevate_status;
    }

    // Different syscall cases will set the
    // return value to be returned at the end.
    long return_val = 0;

    // Try the new dispatch table first for migrated syscalls
    if (syscallnum < MAX_SYSCALL_NUM && g_syscall_table[syscallnum]) {
        return_val = g_syscall_table[syscallnum](arg1, arg2, arg3, arg4, arg5, arg6);
        } else {
        kprint("Unknown syscall number %llu\n", syscallnum);
        return_val = -ENOSYS;
    }

    // Condition under which the SYS_ELEVATE call succeeded and
    // restoration of original elevate status is not necessary.
    bool successfull_elevation_syscall =
        (syscallnum == SYSCALL_SYS_ELEVATE) && (return_val == 0);

    if (!successfull_elevation_syscall) {
        // Restore the original elevate status
        original_task->get_core()->hw_state.elevated = original_elevate_status;
    }

    return return_val;
}

EXTERN_C
long syscall(
    uint64_t syscall_number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
) {
    long ret;

    asm volatile(
        "mov %1, %%rax\n"  // syscall number
        "mov %2, %%rdi\n"  // arg1
        "mov %3, %%rsi\n"  // arg2
        "mov %4, %%rdx\n"  // arg3
        "mov %5, %%r10\n"  // arg4
        "mov %6, %%r8\n"   // arg5
        "mov %7, %%r9\n"   // arg6
        "syscall\n"
        "mov %%rax, %0\n"  // Capture return value
        : "=r"(ret)
        : "r"(syscall_number), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4), "r"(arg5), "r"(arg6)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"
    );

    return static_cast<long>(ret);
}

