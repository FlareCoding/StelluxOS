#ifndef SYS_MEM_H
#define SYS_MEM_H

#include <syscall/syscall_registry.h>

// Declare all memory management syscall handlers
DECLARE_SYSCALL_HANDLER(mmap);
DECLARE_SYSCALL_HANDLER(munmap);
DECLARE_SYSCALL_HANDLER(brk);

#endif // SYS_MEM_H 