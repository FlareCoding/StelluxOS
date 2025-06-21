#ifndef SYSCALL_REGISTRY_H
#define SYSCALL_REGISTRY_H

#include <syscall/syscalls.h>

// Syscall handler function signature
typedef long (*syscall_handler_t)(uint64_t arg1, uint64_t arg2, uint64_t arg3, 
                                  uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Maximum syscall number (adjust as needed)
#define MAX_SYSCALL_NUM 1024

// Macro to declare a syscall handler
#define DECLARE_SYSCALL_HANDLER(name) \
    long sys_##name##_handler([[maybe_unused]] uint64_t arg1, \
                              [[maybe_unused]] uint64_t arg2, \
                              [[maybe_unused]] uint64_t arg3, \
                              [[maybe_unused]] uint64_t arg4, \
                              [[maybe_unused]] uint64_t arg5, \
                              [[maybe_unused]] uint64_t arg6)

// The global syscall table (defined in syscalls.cpp)
extern syscall_handler_t g_syscall_table[MAX_SYSCALL_NUM];

// Syscall tracing macro - can be enabled/disabled with STELLUX_STRACE_ENABLED
#ifdef STELLUX_STRACE_ENABLED
#include <core/klog.h>
#define SYSCALL_TRACE(...) kprint(__VA_ARGS__)
#else
#define SYSCALL_TRACE(...) do { } while(0)
#endif

#endif // SYSCALL_REGISTRY_H
