#include "syscall/syscall_table.h"
#include "syscall/syscall.h"
#include "syscall/linux_syscalls.h"
#include "syscall/handlers/sys_task.h"
#include "syscall/handlers/sys_elevate.h"
#include "syscall/handlers/sys_io.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_mmap.h"
#include "syscall/handlers/sys_socket.h"
#include "syscall/handlers/sys_memfd.h"
#include "syscall/handlers/sys_proc.h"
#include "syscall/handlers/sys_pty.h"
#include "syscall/handlers/sys_clock.h"
#include "syscall/handlers/sys_random.h"
#include "syscall/handlers/sys_poll.h"
#include "syscall/handlers/sys_shutdown.h"
#include "syscall/handlers/sys_sockaddr.h"
#include "syscall/handlers/sys_signal.h"
#include "syscall/handlers/sys_select.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/handlers/sys_uname.h"
#include "syscall/handlers/sys_futex.h"

namespace syscall {

handler_t g_syscall_table[MAX_SYSCALL_NUM];

__PRIVILEGED_CODE void init_syscall_table() {
    for (uint64_t i = 0; i < MAX_SYSCALL_NUM; i++)
        g_syscall_table[i] = nullptr;

    REGISTER_SYSCALL(linux_nr::IOCTL,           ioctl);
    REGISTER_SYSCALL(linux_nr::PREAD64,         pread64);
    REGISTER_SYSCALL(linux_nr::WRITEV,          writev);
    REGISTER_SYSCALL(linux_nr::READ,            read);
    REGISTER_SYSCALL(linux_nr::WRITE,           write);
    REGISTER_SYSCALL(linux_nr::CLOSE,           close);
    REGISTER_SYSCALL(linux_nr::LSEEK,           lseek);
    REGISTER_SYSCALL(linux_nr::FSTAT,           fstat);
    REGISTER_SYSCALL(linux_nr::NEWFSTATAT,      newfstatat);
    REGISTER_SYSCALL(linux_nr::GETDENTS64,      getdents64);
    REGISTER_SYSCALL(linux_nr::GETCWD,          getcwd);
    REGISTER_SYSCALL(linux_nr::CHDIR,           chdir);
    REGISTER_SYSCALL(linux_nr::FCHDIR,          fchdir);
    REGISTER_SYSCALL(linux_nr::OPENAT,          openat);
#if defined(__x86_64__)
    REGISTER_SYSCALL(linux_nr::PIPE,            pipe);
    REGISTER_SYSCALL(linux_nr::POLL,            poll);
    REGISTER_SYSCALL(linux_nr::SELECT,          select);
    REGISTER_SYSCALL(linux_nr::OPEN,            open);
    REGISTER_SYSCALL(linux_nr::STAT,            stat);
#endif
    REGISTER_SYSCALL(linux_nr::BRK,             brk);
    REGISTER_SYSCALL(linux_nr::MADVISE,         madvise);
    REGISTER_SYSCALL(linux_nr::MMAP,            mmap);
    REGISTER_SYSCALL(linux_nr::RT_SIGACTION,    rt_sigaction);
    REGISTER_SYSCALL(linux_nr::RT_SIGPROCMASK,  rt_sigprocmask);
    REGISTER_SYSCALL(linux_nr::MPROTECT,        mprotect);
    REGISTER_SYSCALL(linux_nr::MUNMAP,          munmap);
    REGISTER_SYSCALL(linux_nr::EXIT,            exit);
    REGISTER_SYSCALL(linux_nr::EXIT_GROUP,      exit_group);
    REGISTER_SYSCALL(linux_nr::GETPID,          getpid);
    REGISTER_SYSCALL(linux_nr::GETUID,          getuid);
    REGISTER_SYSCALL(linux_nr::GETEUID,         geteuid);
    REGISTER_SYSCALL(linux_nr::GETGID,          getgid);
    REGISTER_SYSCALL(linux_nr::GETEGID,         getegid);
    REGISTER_SYSCALL(linux_nr::SET_TID_ADDRESS, set_tid_address);
    REGISTER_SYSCALL(linux_nr::NANOSLEEP,       nanosleep);
    REGISTER_SYSCALL(linux_nr::CLOCK_GETTIME,   clock_gettime);
    REGISTER_SYSCALL(linux_nr::CLOCK_GETRES,    clock_getres);
    REGISTER_SYSCALL(linux_nr::GETTIMEOFDAY,    gettimeofday);
    REGISTER_SYSCALL(linux_nr::SYSINFO,         sysinfo);
    REGISTER_SYSCALL(linux_nr::UNAME,           uname);
    REGISTER_SYSCALL(linux_nr::SCHED_GETAFFINITY, sched_getaffinity);

    REGISTER_SYSCALL(linux_nr::SOCKET,      socket);
    REGISTER_SYSCALL(linux_nr::SOCKETPAIR,  socketpair);
    REGISTER_SYSCALL(linux_nr::BIND,        bind);
    REGISTER_SYSCALL(linux_nr::LISTEN,      listen);
    REGISTER_SYSCALL(linux_nr::ACCEPT,      accept);
    REGISTER_SYSCALL(linux_nr::CONNECT,     connect);
    REGISTER_SYSCALL(linux_nr::SENDTO,      sendto);
    REGISTER_SYSCALL(linux_nr::RECVFROM,    recvfrom);
    REGISTER_SYSCALL(linux_nr::SETSOCKOPT,  setsockopt);
    REGISTER_SYSCALL(linux_nr::GETSOCKOPT,  getsockopt);
    REGISTER_SYSCALL(linux_nr::GETSOCKNAME, getsockname);
    REGISTER_SYSCALL(linux_nr::GETPEERNAME, getpeername);
    REGISTER_SYSCALL(linux_nr::SHUTDOWN,    shutdown);
    REGISTER_SYSCALL(linux_nr::FCNTL,       fcntl);
    REGISTER_SYSCALL(linux_nr::GETRANDOM,   getrandom);
    REGISTER_SYSCALL(linux_nr::PSELECT6,    pselect6);
    REGISTER_SYSCALL(linux_nr::PPOLL,       ppoll);
    REGISTER_SYSCALL(linux_nr::PIPE2,      pipe2);

    REGISTER_SYSCALL(linux_nr::MEMFD_CREATE, memfd_create);
    REGISTER_SYSCALL(linux_nr::FSYNC,       fsync);
    REGISTER_SYSCALL(linux_nr::FTRUNCATE,   ftruncate);
    REGISTER_SYSCALL(linux_nr::MKDIRAT,     mkdirat);
    REGISTER_SYSCALL(linux_nr::UNLINKAT,    unlinkat);
#if defined(__x86_64__)
    REGISTER_SYSCALL(linux_nr::MKDIR,       mkdir);
    REGISTER_SYSCALL(linux_nr::UNLINK,      unlink);
    REGISTER_SYSCALL(linux_nr::RMDIR,       rmdir);
#endif

    REGISTER_SYSCALL(SYS_ELEVATE, elevate);

    REGISTER_SYSCALL(SYS_PROC_CREATE, proc_create);
    REGISTER_SYSCALL(SYS_PROC_START,  proc_start);
    REGISTER_SYSCALL(SYS_PROC_WAIT,   proc_wait);
    REGISTER_SYSCALL(SYS_PROC_DETACH, proc_detach);
    REGISTER_SYSCALL(SYS_PROC_INFO,   proc_info);
    REGISTER_SYSCALL(SYS_PROC_SET_HANDLE, proc_set_handle);
    REGISTER_SYSCALL(SYS_PROC_KILL, proc_kill);
    REGISTER_SYSCALL(SYS_PROC_THREAD_CREATE, proc_create_thread);
    REGISTER_SYSCALL(SYS_PROC_KILL_TID, proc_kill_tid);

    REGISTER_SYSCALL(SYS_PTY_CREATE, pty_create);

    REGISTER_SYSCALL(SYS_FUTEX_WAIT,     futex_wait);
    REGISTER_SYSCALL(SYS_FUTEX_WAKE,     futex_wake);
    REGISTER_SYSCALL(SYS_FUTEX_WAKE_ALL, futex_wake_all);

    register_arch_syscalls();
}

} // namespace syscall
