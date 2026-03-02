#include "syscall/syscall_table.h"
#include "syscall/syscall.h"
#include "syscall/linux_syscalls.h"
#include "syscall/handlers/sys_task.h"
#include "syscall/handlers/sys_elevate.h"
#include "syscall/handlers/sys_io.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_mmap.h"
#include "syscall/handlers/sys_socket.h"

namespace syscall {

handler_t g_syscall_table[MAX_SYSCALL_NUM];

__PRIVILEGED_CODE void init_syscall_table() {
    for (uint64_t i = 0; i < MAX_SYSCALL_NUM; i++)
        g_syscall_table[i] = nullptr;

    REGISTER_SYSCALL(linux_nr::IOCTL,           ioctl);
    REGISTER_SYSCALL(linux_nr::WRITEV,          writev);
    REGISTER_SYSCALL(linux_nr::READ,            read);
    REGISTER_SYSCALL(linux_nr::WRITE,           write);
    REGISTER_SYSCALL(linux_nr::CLOSE,           close);
    REGISTER_SYSCALL(linux_nr::OPENAT,          openat);
#if defined(__x86_64__)
    REGISTER_SYSCALL(linux_nr::OPEN,            open);
#endif
    REGISTER_SYSCALL(linux_nr::MMAP,            mmap);
    REGISTER_SYSCALL(linux_nr::MPROTECT,        mprotect);
    REGISTER_SYSCALL(linux_nr::MUNMAP,          munmap);
    REGISTER_SYSCALL(linux_nr::EXIT,            exit);
    REGISTER_SYSCALL(linux_nr::EXIT_GROUP,      exit_group);
    REGISTER_SYSCALL(linux_nr::SET_TID_ADDRESS, set_tid_address);

    REGISTER_SYSCALL(linux_nr::SOCKET,      socket);
    REGISTER_SYSCALL(linux_nr::SOCKETPAIR,  socketpair);
    REGISTER_SYSCALL(linux_nr::BIND,        bind);
    REGISTER_SYSCALL(linux_nr::LISTEN,      listen);
    REGISTER_SYSCALL(linux_nr::ACCEPT,      accept);
    REGISTER_SYSCALL(linux_nr::CONNECT,     connect);
    REGISTER_SYSCALL(linux_nr::FCNTL,       fcntl);

    REGISTER_SYSCALL(SYS_ELEVATE, elevate);

    register_arch_syscalls();
}

} // namespace syscall
