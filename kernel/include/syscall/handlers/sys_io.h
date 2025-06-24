#ifndef SYS_IO_H
#define SYS_IO_H

#include <syscall/syscall_registry.h>

// Declare all I/O-related syscall handlers
DECLARE_SYSCALL_HANDLER(read);
DECLARE_SYSCALL_HANDLER(write);
DECLARE_SYSCALL_HANDLER(writev);
DECLARE_SYSCALL_HANDLER(open);
DECLARE_SYSCALL_HANDLER(close);
DECLARE_SYSCALL_HANDLER(fcntl);
DECLARE_SYSCALL_HANDLER(lseek);
DECLARE_SYSCALL_HANDLER(ioctl);

#endif // SYS_IO_H 