#ifndef SYS_SHM_H
#define SYS_SHM_H

#include <syscall/syscall_registry.h>

// Declare all shared memory-related syscall handlers
DECLARE_SYSCALL_HANDLER(shm_create);
DECLARE_SYSCALL_HANDLER(shm_open);
DECLARE_SYSCALL_HANDLER(shm_destroy);
DECLARE_SYSCALL_HANDLER(shm_map);
DECLARE_SYSCALL_HANDLER(shm_unmap);

#endif // SYS_SHM_H
