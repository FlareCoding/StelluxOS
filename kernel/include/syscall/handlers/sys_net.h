#ifndef SYS_NET_H
#define SYS_NET_H

#include <syscall/syscall_registry.h>

// Declare all network-related syscall handlers
DECLARE_SYSCALL_HANDLER(socket);
DECLARE_SYSCALL_HANDLER(connect);
DECLARE_SYSCALL_HANDLER(accept);
DECLARE_SYSCALL_HANDLER(bind);
DECLARE_SYSCALL_HANDLER(listen);

#endif // SYS_NET_H
