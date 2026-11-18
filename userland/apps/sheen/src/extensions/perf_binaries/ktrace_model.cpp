#include "extensions/perf_binaries/ktrace_model.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <unordered_map>

namespace sheen {

static constexpr std::string_view KTRACE_CAPTURE_MAGIC = "STLXKTRC";
static constexpr size_t SQLITE_HEADER_PREFIX_BYTES = 16;
static constexpr char SQLITE_HEADER_MAGIC[SQLITE_HEADER_PREFIX_BYTES] = "SQLite format 3";

static constexpr uint16_t EM_X86_64_CODE = 62, EM_AARCH64_CODE = 183;
static constexpr int64_t NEGATED_ERRNO_MIN = -4095, NEGATED_ERRNO_MAX = -1;
static constexpr uint8_t PRINTABLE_ASCII_MIN = 0x20, PRINTABLE_ASCII_MAX = 0x7e;
static constexpr uint64_t NS_IN_SEC = 1000000000ull, NS_IN_MSEC = 1000000ull, NS_IN_USEC = 1000ull;
static constexpr double PERCENT_HUNDRED = 100.0;
static constexpr std::string_view RECORDER_LEADER_NAME = "ktrace";

namespace {

using sqlite_db_ptr = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;
using sqlite_stmt_ptr = std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)>;

struct numbered_name { uint32_t code; const char* name; };
struct errno_entry { int32_t code; const char* name; };
struct time_interval { uint64_t start_ns = 0, end_ns = 0; };

struct flame_tree_node {
    std::string name;
    uint32_t depth = 0;
    uint64_t total_ns = 0, self_ns = 0, call_count = 0;
    std::map<std::string, size_t> child_by_name;
    std::vector<size_t> children;
};

} // namespace

static constexpr numbered_name STELLUX_SYSCALL_TABLE[] = {
    {1010,"proc_create"}, {1011,"proc_start"}, {1012,"proc_wait"}, {1013,"proc_detach"}, {1014,"proc_info"}, {1015,"proc_set_handle"}, {1016,"proc_kill"},
    {1017,"proc_thread_create"}, {1020,"pty_create"}, {1030,"futex_wait"}, {1031,"futex_wake"}, {1032,"futex_wake_all"}, {1040,"handle_restrict"},
};

static constexpr numbered_name X86_64_SYSCALL_TABLE[] = {
    {0,"read"}, {1,"write"}, {2,"open"}, {3,"close"}, {4,"stat"}, {5,"fstat"}, {6,"lstat"}, {7,"poll"}, {8,"lseek"}, {9,"mmap"}, {10,"mprotect"}, {11,"munmap"},
    {12,"brk"}, {13,"rt_sigaction"}, {14,"rt_sigprocmask"}, {15,"rt_sigreturn"}, {16,"ioctl"}, {17,"pread64"}, {18,"pwrite64"}, {19,"readv"}, {20,"writev"}, {21,"access"}, {22,"pipe"}, {23,"select"},
    {24,"sched_yield"}, {25,"mremap"}, {26,"msync"}, {27,"mincore"}, {28,"madvise"}, {29,"shmget"}, {30,"shmat"}, {31,"shmctl"}, {32,"dup"}, {33,"dup2"}, {34,"pause"}, {35,"nanosleep"},
    {36,"getitimer"}, {37,"alarm"}, {38,"setitimer"}, {39,"getpid"}, {40,"sendfile"}, {41,"socket"}, {42,"connect"}, {43,"accept"}, {44,"sendto"}, {45,"recvfrom"}, {46,"sendmsg"}, {47,"recvmsg"},
    {48,"shutdown"}, {49,"bind"}, {50,"listen"}, {51,"getsockname"}, {52,"getpeername"}, {53,"socketpair"}, {54,"setsockopt"}, {55,"getsockopt"}, {56,"clone"}, {57,"fork"}, {58,"vfork"}, {59,"execve"},
    {60,"exit"}, {61,"wait4"}, {62,"kill"}, {63,"uname"}, {64,"semget"}, {65,"semop"}, {66,"semctl"}, {67,"shmdt"}, {68,"msgget"}, {69,"msgsnd"}, {70,"msgrcv"}, {71,"msgctl"},
    {72,"fcntl"}, {73,"flock"}, {74,"fsync"}, {75,"fdatasync"}, {76,"truncate"}, {77,"ftruncate"}, {78,"getdents"}, {79,"getcwd"}, {80,"chdir"}, {81,"fchdir"}, {82,"rename"}, {83,"mkdir"},
    {84,"rmdir"}, {85,"creat"}, {86,"link"}, {87,"unlink"}, {88,"symlink"}, {89,"readlink"}, {90,"chmod"}, {91,"fchmod"}, {92,"chown"}, {93,"fchown"}, {94,"lchown"}, {95,"umask"},
    {96,"gettimeofday"}, {97,"getrlimit"}, {98,"getrusage"}, {99,"sysinfo"}, {100,"times"}, {101,"ptrace"}, {102,"getuid"}, {103,"syslog"}, {104,"getgid"}, {105,"setuid"}, {106,"setgid"}, {107,"geteuid"},
    {108,"getegid"}, {109,"setpgid"}, {110,"getppid"}, {111,"getpgrp"}, {112,"setsid"}, {113,"setreuid"}, {114,"setregid"}, {115,"getgroups"}, {116,"setgroups"}, {117,"setresuid"}, {118,"getresuid"}, {119,"setresgid"},
    {120,"getresgid"}, {121,"getpgid"}, {122,"setfsuid"}, {123,"setfsgid"}, {124,"getsid"}, {125,"capget"}, {126,"capset"}, {127,"rt_sigpending"}, {128,"rt_sigtimedwait"}, {129,"rt_sigqueueinfo"}, {130,"rt_sigsuspend"}, {131,"sigaltstack"},
    {132,"utime"}, {133,"mknod"}, {134,"uselib"}, {135,"personality"}, {136,"ustat"}, {137,"statfs"}, {138,"fstatfs"}, {139,"sysfs"}, {140,"getpriority"}, {141,"setpriority"}, {142,"sched_setparam"}, {143,"sched_getparam"},
    {144,"sched_setscheduler"}, {145,"sched_getscheduler"}, {146,"sched_get_priority_max"}, {147,"sched_get_priority_min"}, {148,"sched_rr_get_interval"}, {149,"mlock"}, {150,"munlock"}, {151,"mlockall"}, {152,"munlockall"}, {153,"vhangup"}, {154,"modify_ldt"}, {155,"pivot_root"},
    {156,"_sysctl"}, {157,"prctl"}, {158,"arch_prctl"}, {159,"adjtimex"}, {160,"setrlimit"}, {161,"chroot"}, {162,"sync"}, {163,"acct"}, {164,"settimeofday"}, {165,"mount"}, {166,"umount2"}, {167,"swapon"},
    {168,"swapoff"}, {169,"reboot"}, {170,"sethostname"}, {171,"setdomainname"}, {172,"iopl"}, {173,"ioperm"}, {174,"create_module"}, {175,"init_module"}, {176,"delete_module"}, {177,"get_kernel_syms"}, {178,"query_module"}, {179,"quotactl"},
    {180,"nfsservctl"}, {181,"getpmsg"}, {182,"putpmsg"}, {183,"afs_syscall"}, {184,"tuxcall"}, {185,"security"}, {186,"gettid"}, {187,"readahead"}, {188,"setxattr"}, {189,"lsetxattr"}, {190,"fsetxattr"}, {191,"getxattr"},
    {192,"lgetxattr"}, {193,"fgetxattr"}, {194,"listxattr"}, {195,"llistxattr"}, {196,"flistxattr"}, {197,"removexattr"}, {198,"lremovexattr"}, {199,"fremovexattr"}, {200,"tkill"}, {201,"time"}, {202,"futex"}, {203,"sched_setaffinity"},
    {204,"sched_getaffinity"}, {205,"set_thread_area"}, {206,"io_setup"}, {207,"io_destroy"}, {208,"io_getevents"}, {209,"io_submit"}, {210,"io_cancel"}, {211,"get_thread_area"}, {212,"lookup_dcookie"}, {213,"epoll_create"}, {214,"epoll_ctl_old"}, {215,"epoll_wait_old"},
    {216,"remap_file_pages"}, {217,"getdents64"}, {218,"set_tid_address"}, {219,"restart_syscall"}, {220,"semtimedop"}, {221,"fadvise64"}, {222,"timer_create"}, {223,"timer_settime"}, {224,"timer_gettime"}, {225,"timer_getoverrun"}, {226,"timer_delete"}, {227,"clock_settime"},
    {228,"clock_gettime"}, {229,"clock_getres"}, {230,"clock_nanosleep"}, {231,"exit_group"}, {232,"epoll_wait"}, {233,"epoll_ctl"}, {234,"tgkill"}, {235,"utimes"}, {236,"vserver"}, {237,"mbind"}, {238,"set_mempolicy"}, {239,"get_mempolicy"},
    {240,"mq_open"}, {241,"mq_unlink"}, {242,"mq_timedsend"}, {243,"mq_timedreceive"}, {244,"mq_notify"}, {245,"mq_getsetattr"}, {246,"kexec_load"}, {247,"waitid"}, {248,"add_key"}, {249,"request_key"}, {250,"keyctl"}, {251,"ioprio_set"},
    {252,"ioprio_get"}, {253,"inotify_init"}, {254,"inotify_add_watch"}, {255,"inotify_rm_watch"}, {256,"migrate_pages"}, {257,"openat"}, {258,"mkdirat"}, {259,"mknodat"}, {260,"fchownat"}, {261,"futimesat"}, {262,"newfstatat"}, {263,"unlinkat"},
    {264,"renameat"}, {265,"linkat"}, {266,"symlinkat"}, {267,"readlinkat"}, {268,"fchmodat"}, {269,"faccessat"}, {270,"pselect6"}, {271,"ppoll"}, {272,"unshare"}, {273,"set_robust_list"}, {274,"get_robust_list"}, {275,"splice"},
    {276,"tee"}, {277,"sync_file_range"}, {278,"vmsplice"}, {279,"move_pages"}, {280,"utimensat"}, {281,"epoll_pwait"}, {282,"signalfd"}, {283,"timerfd_create"}, {284,"eventfd"}, {285,"fallocate"}, {286,"timerfd_settime"}, {287,"timerfd_gettime"},
    {288,"accept4"}, {289,"signalfd4"}, {290,"eventfd2"}, {291,"epoll_create1"}, {292,"dup3"}, {293,"pipe2"}, {294,"inotify_init1"}, {295,"preadv"}, {296,"pwritev"}, {297,"rt_tgsigqueueinfo"}, {298,"perf_event_open"}, {299,"recvmmsg"},
    {300,"fanotify_init"}, {301,"fanotify_mark"}, {302,"prlimit64"}, {303,"name_to_handle_at"}, {304,"open_by_handle_at"}, {305,"clock_adjtime"}, {306,"syncfs"}, {307,"sendmmsg"}, {308,"setns"}, {309,"getcpu"}, {310,"process_vm_readv"}, {311,"process_vm_writev"},
    {312,"kcmp"}, {313,"finit_module"}, {314,"sched_setattr"}, {315,"sched_getattr"}, {316,"renameat2"}, {317,"seccomp"}, {318,"getrandom"}, {319,"memfd_create"}, {320,"kexec_file_load"}, {321,"bpf"}, {322,"execveat"}, {323,"userfaultfd"},
    {324,"membarrier"}, {325,"mlock2"}, {326,"copy_file_range"}, {327,"preadv2"}, {328,"pwritev2"}, {329,"pkey_mprotect"}, {330,"pkey_alloc"}, {331,"pkey_free"}, {332,"statx"}, {333,"io_pgetevents"}, {334,"rseq"}, {424,"pidfd_send_signal"},
    {425,"io_uring_setup"}, {426,"io_uring_enter"}, {427,"io_uring_register"}, {428,"open_tree"}, {429,"move_mount"}, {430,"fsopen"}, {431,"fsconfig"}, {432,"fsmount"}, {433,"fspick"}, {434,"pidfd_open"}, {435,"clone3"}, {436,"close_range"},
    {437,"openat2"}, {438,"pidfd_getfd"}, {439,"faccessat2"}, {440,"process_madvise"}, {441,"epoll_pwait2"}, {442,"mount_setattr"}, {444,"landlock_create_ruleset"}, {445,"landlock_add_rule"}, {446,"landlock_restrict_self"}, {447,"memfd_secret"}, {448,"process_mrelease"}, {449,"futex_waitv"},
    {450,"set_mempolicy_home_node"}, {451,"cachestat"}, {452,"fchmodat2"},
};

static constexpr numbered_name AARCH64_SYSCALL_TABLE[] = {
    {0,"io_setup"}, {1,"io_destroy"}, {2,"io_submit"}, {3,"io_cancel"}, {4,"io_getevents"}, {5,"setxattr"}, {6,"lsetxattr"}, {7,"fsetxattr"}, {8,"getxattr"}, {9,"lgetxattr"}, {10,"fgetxattr"}, {11,"listxattr"},
    {12,"llistxattr"}, {13,"flistxattr"}, {14,"removexattr"}, {15,"lremovexattr"}, {16,"fremovexattr"}, {17,"getcwd"}, {18,"lookup_dcookie"}, {19,"eventfd2"}, {20,"epoll_create1"}, {21,"epoll_ctl"}, {22,"epoll_pwait"}, {23,"dup"},
    {24,"dup3"}, {25,"fcntl"}, {26,"inotify_init1"}, {27,"inotify_add_watch"}, {28,"inotify_rm_watch"}, {29,"ioctl"}, {30,"ioprio_set"}, {31,"ioprio_get"}, {32,"flock"}, {33,"mknodat"}, {34,"mkdirat"}, {35,"unlinkat"},
    {36,"symlinkat"}, {37,"linkat"}, {38,"renameat"}, {39,"umount2"}, {40,"mount"}, {41,"pivot_root"}, {42,"nfsservctl"}, {43,"statfs"}, {44,"fstatfs"}, {45,"truncate"}, {46,"ftruncate"}, {47,"fallocate"},
    {48,"faccessat"}, {49,"chdir"}, {50,"fchdir"}, {51,"chroot"}, {52,"fchmod"}, {53,"fchmodat"}, {54,"fchownat"}, {55,"fchown"}, {56,"openat"}, {57,"close"}, {58,"vhangup"}, {59,"pipe2"},
    {60,"quotactl"}, {61,"getdents64"}, {62,"lseek"}, {63,"read"}, {64,"write"}, {65,"readv"}, {66,"writev"}, {67,"pread64"}, {68,"pwrite64"}, {69,"preadv"}, {70,"pwritev"}, {71,"sendfile"},
    {72,"pselect6"}, {73,"ppoll"}, {74,"signalfd4"}, {75,"vmsplice"}, {76,"splice"}, {77,"tee"}, {78,"readlinkat"}, {79,"newfstatat"}, {80,"fstat"}, {81,"sync"}, {82,"fsync"}, {83,"fdatasync"},
    {84,"sync_file_range"}, {85,"timerfd_create"}, {86,"timerfd_settime"}, {87,"timerfd_gettime"}, {88,"utimensat"}, {89,"acct"}, {90,"capget"}, {91,"capset"}, {92,"personality"}, {93,"exit"}, {94,"exit_group"}, {95,"waitid"},
    {96,"set_tid_address"}, {97,"unshare"}, {98,"futex"}, {99,"set_robust_list"}, {100,"get_robust_list"}, {101,"nanosleep"}, {102,"getitimer"}, {103,"setitimer"}, {104,"kexec_load"}, {105,"init_module"}, {106,"delete_module"}, {107,"timer_create"},
    {108,"timer_gettime"}, {109,"timer_getoverrun"}, {110,"timer_settime"}, {111,"timer_delete"}, {112,"clock_settime"}, {113,"clock_gettime"}, {114,"clock_getres"}, {115,"clock_nanosleep"}, {116,"syslog"}, {117,"ptrace"}, {118,"sched_setparam"}, {119,"sched_setscheduler"},
    {120,"sched_getscheduler"}, {121,"sched_getparam"}, {122,"sched_setaffinity"}, {123,"sched_getaffinity"}, {124,"sched_yield"}, {125,"sched_get_priority_max"}, {126,"sched_get_priority_min"}, {127,"sched_rr_get_interval"}, {128,"restart_syscall"}, {129,"kill"}, {130,"tkill"}, {131,"tgkill"},
    {132,"sigaltstack"}, {133,"rt_sigsuspend"}, {134,"rt_sigaction"}, {135,"rt_sigprocmask"}, {136,"rt_sigpending"}, {137,"rt_sigtimedwait"}, {138,"rt_sigqueueinfo"}, {139,"rt_sigreturn"}, {140,"setpriority"}, {141,"getpriority"}, {142,"reboot"}, {143,"setregid"},
    {144,"setgid"}, {145,"setreuid"}, {146,"setuid"}, {147,"setresuid"}, {148,"getresuid"}, {149,"setresgid"}, {150,"getresgid"}, {151,"setfsuid"}, {152,"setfsgid"}, {153,"times"}, {154,"setpgid"}, {155,"getpgid"},
    {156,"getsid"}, {157,"setsid"}, {158,"getgroups"}, {159,"setgroups"}, {160,"uname"}, {161,"sethostname"}, {162,"setdomainname"}, {163,"getrlimit"}, {164,"setrlimit"}, {165,"getrusage"}, {166,"umask"}, {167,"prctl"},
    {168,"getcpu"}, {169,"gettimeofday"}, {170,"settimeofday"}, {171,"adjtimex"}, {172,"getpid"}, {173,"getppid"}, {174,"getuid"}, {175,"geteuid"}, {176,"getgid"}, {177,"getegid"}, {178,"gettid"}, {179,"sysinfo"},
    {180,"mq_open"}, {181,"mq_unlink"}, {182,"mq_timedsend"}, {183,"mq_timedreceive"}, {184,"mq_notify"}, {185,"mq_getsetattr"}, {186,"msgget"}, {187,"msgctl"}, {188,"msgrcv"}, {189,"msgsnd"}, {190,"semget"}, {191,"semctl"},
    {192,"semtimedop"}, {193,"semop"}, {194,"shmget"}, {195,"shmctl"}, {196,"shmat"}, {197,"shmdt"}, {198,"socket"}, {199,"socketpair"}, {200,"bind"}, {201,"listen"}, {202,"accept"}, {203,"connect"},
    {204,"getsockname"}, {205,"getpeername"}, {206,"sendto"}, {207,"recvfrom"}, {208,"setsockopt"}, {209,"getsockopt"}, {210,"shutdown"}, {211,"sendmsg"}, {212,"recvmsg"}, {213,"readahead"}, {214,"brk"}, {215,"munmap"},
    {216,"mremap"}, {217,"add_key"}, {218,"request_key"}, {219,"keyctl"}, {220,"clone"}, {221,"execve"}, {222,"mmap"}, {223,"fadvise64"}, {224,"swapon"}, {225,"swapoff"}, {226,"mprotect"}, {227,"msync"},
    {228,"mlock"}, {229,"munlock"}, {230,"mlockall"}, {231,"munlockall"}, {232,"mincore"}, {233,"madvise"}, {234,"remap_file_pages"}, {235,"mbind"}, {236,"get_mempolicy"}, {237,"set_mempolicy"}, {238,"migrate_pages"}, {239,"move_pages"},
    {240,"rt_tgsigqueueinfo"}, {241,"perf_event_open"}, {242,"accept4"}, {243,"recvmmsg"}, {260,"wait4"}, {261,"prlimit64"}, {262,"fanotify_init"}, {263,"fanotify_mark"}, {264,"name_to_handle_at"}, {265,"open_by_handle_at"}, {266,"clock_adjtime"}, {267,"syncfs"},
    {268,"setns"}, {269,"sendmmsg"}, {270,"process_vm_readv"}, {271,"process_vm_writev"}, {272,"kcmp"}, {273,"finit_module"}, {274,"sched_setattr"}, {275,"sched_getattr"}, {276,"renameat2"}, {277,"seccomp"}, {278,"getrandom"}, {279,"memfd_create"},
    {280,"bpf"}, {281,"execveat"}, {282,"userfaultfd"}, {283,"membarrier"}, {284,"mlock2"}, {285,"copy_file_range"}, {286,"preadv2"}, {287,"pwritev2"}, {288,"pkey_mprotect"}, {289,"pkey_alloc"}, {290,"pkey_free"}, {291,"statx"},
    {292,"io_pgetevents"}, {293,"rseq"}, {294,"kexec_file_load"}, {424,"pidfd_send_signal"}, {425,"io_uring_setup"}, {426,"io_uring_enter"}, {427,"io_uring_register"}, {428,"open_tree"}, {429,"move_mount"}, {430,"fsopen"}, {431,"fsconfig"}, {432,"fsmount"},
    {433,"fspick"}, {434,"pidfd_open"}, {435,"clone3"}, {436,"close_range"}, {437,"openat2"}, {438,"pidfd_getfd"}, {439,"faccessat2"}, {440,"process_madvise"}, {441,"epoll_pwait2"}, {442,"mount_setattr"}, {444,"landlock_create_ruleset"}, {445,"landlock_add_rule"},
    {446,"landlock_restrict_self"}, {448,"process_mrelease"}, {449,"futex_waitv"}, {450,"set_mempolicy_home_node"}, {451,"cachestat"}, {452,"fchmodat2"},
};

static constexpr errno_entry STELLUX_ERRNO_TABLE[] = {
    {1,"EPERM"}, {2,"ENOENT"}, {3,"ESRCH"}, {4,"EINTR"}, {5,"EIO"}, {6,"ENXIO"}, {7,"E2BIG"}, {8,"ENOEXEC"}, {9,"EBADF"}, {10,"ECHILD"}, {11,"EAGAIN"}, {12,"ENOMEM"},
    {13,"EACCES"}, {14,"EFAULT"}, {15,"ENOTBLK"}, {16,"EBUSY"}, {17,"EEXIST"}, {18,"EXDEV"}, {19,"ENODEV"}, {20,"ENOTDIR"}, {21,"EISDIR"}, {22,"EINVAL"}, {23,"ENFILE"}, {24,"EMFILE"},
    {25,"ENOTTY"}, {26,"ETXTBSY"}, {27,"EFBIG"}, {28,"ENOSPC"}, {29,"ESPIPE"}, {30,"EROFS"}, {31,"EMLINK"}, {32,"EPIPE"}, {33,"EDOM"}, {34,"ERANGE"}, {35,"EDEADLK"}, {36,"ENAMETOOLONG"},
    {37,"ENOLCK"}, {38,"ENOSYS"}, {39,"ENOTEMPTY"}, {40,"ELOOP"}, {42,"ENOMSG"}, {43,"EIDRM"}, {44,"ECHRNG"}, {45,"EL2NSYNC"}, {46,"EL3HLT"}, {47,"EL3RST"}, {48,"ELNRNG"}, {49,"EUNATCH"},
    {50,"ENOCSI"}, {51,"EL2HLT"}, {52,"EBADE"}, {53,"EBADR"}, {54,"EXFULL"}, {55,"ENOANO"}, {56,"EBADRQC"}, {57,"EBADSLT"}, {59,"EBFONT"}, {60,"ENOSTR"}, {61,"ENODATA"}, {62,"ETIME"},
    {63,"ENOSR"}, {64,"ENONET"}, {65,"ENOPKG"}, {66,"EREMOTE"}, {67,"ENOLINK"}, {68,"EADV"}, {69,"ESRMNT"}, {70,"ECOMM"}, {71,"EPROTO"}, {72,"EMULTIHOP"}, {73,"EDOTDOT"}, {74,"EBADMSG"},
    {75,"EOVERFLOW"}, {76,"ENOTUNIQ"}, {77,"EBADFD"}, {78,"EREMCHG"}, {79,"ELIBACC"}, {80,"ELIBBAD"}, {81,"ELIBSCN"}, {82,"ELIBMAX"}, {83,"ELIBEXEC"}, {84,"EILSEQ"}, {85,"ERESTART"}, {86,"ESTRPIPE"},
    {87,"EUSERS"}, {88,"ENOTSOCK"}, {89,"EDESTADDRREQ"}, {90,"EMSGSIZE"}, {91,"EPROTOTYPE"}, {92,"ENOPROTOOPT"}, {93,"EPROTONOSUPPORT"}, {94,"ESOCKTNOSUPPORT"}, {95,"EOPNOTSUPP"}, {96,"EPFNOSUPPORT"}, {97,"EAFNOSUPPORT"}, {98,"EADDRINUSE"},
    {99,"EADDRNOTAVAIL"}, {100,"ENETDOWN"}, {101,"ENETUNREACH"}, {102,"ENETRESET"}, {103,"ECONNABORTED"}, {104,"ECONNRESET"}, {105,"ENOBUFS"}, {106,"EISCONN"}, {107,"ENOTCONN"}, {108,"ESHUTDOWN"}, {109,"ETOOMANYREFS"}, {110,"ETIMEDOUT"},
    {111,"ECONNREFUSED"}, {112,"EHOSTDOWN"}, {113,"EHOSTUNREACH"}, {114,"EALREADY"}, {115,"EINPROGRESS"}, {116,"ESTALE"}, {117,"EUCLEAN"}, {118,"ENOTNAM"}, {119,"ENAVAIL"}, {120,"EISNAM"}, {121,"EREMOTEIO"}, {122,"EDQUOT"},
    {123,"ENOMEDIUM"}, {124,"EMEDIUMTYPE"}, {125,"ECANCELED"}, {126,"ENOKEY"}, {127,"EKEYEXPIRED"}, {128,"EKEYREVOKED"}, {129,"EKEYREJECTED"}, {130,"EOWNERDEAD"}, {131,"ENOTRECOVERABLE"}, {132,"ERFKILL"}, {133,"EHWPOISON"},
};

static uint64_t saturating_add_u64(uint64_t lhs, uint64_t rhs) {
    return rhs > UINT64_MAX - lhs ? UINT64_MAX : (lhs + rhs);
}

static std::string sanitize_ktrace_text(const unsigned char* raw, int raw_len) {
    if (raw == nullptr || raw_len <= 0) {
        return {};
    }

    size_t length = static_cast<size_t>(raw_len);
    std::string cleaned;
    cleaned.reserve(length);
    for (size_t idx = 0; idx < length;) {
        uint8_t lead = raw[idx];
        if (lead >= PRINTABLE_ASCII_MIN && lead <= PRINTABLE_ASCII_MAX) {
            cleaned += static_cast<char>(lead);
            idx++;
            continue;
        }

        size_t seq = (lead & 0xe0u) == 0xc0u ? 2 : ((lead & 0xf0u) == 0xe0u ? 3 : ((lead & 0xf8u) == 0xf0u ? 4 : 0));
        uint32_t cp = seq == 2 ? (lead & 0x1fu) : (seq == 3 ? (lead & 0x0fu) : (lead & 0x07u));
        uint32_t min_cp = seq == 2 ? 0x80u : (seq == 3 ? 0x800u : 0x10000u);
        bool valid = seq > 0 && idx + seq <= length;
        for (size_t k = 1; valid && k < seq; k++) {
            valid = (raw[idx + k] & 0xc0u) == 0x80u;
            cp = (cp << 6) | (raw[idx + k] & 0x3fu);
        }

        if (valid && cp >= min_cp && cp <= 0x10ffffu && (cp < 0xd800u || cp > 0xdfffu)) {
            cleaned.append(reinterpret_cast<const char*>(raw + idx), seq);
            idx += seq;
        } else {
            cleaned += '?';
            idx++;
        }
    }

    return cleaned;
}

static std::string column_string(sqlite3_stmt* stmt, int col) {
    return sanitize_ktrace_text(sqlite3_column_text(stmt, col), sqlite3_column_bytes(stmt, col));
}

static uint64_t column_raw_bitcast_u64(sqlite3_stmt* stmt, int col) {
    return static_cast<uint64_t>(sqlite3_column_int64(stmt, col));
}

template <typename UintT>
static bool column_nonneg_bounded(sqlite3_stmt* stmt, int col, sqlite3_int64 max_val, UintT& out_val) {
    sqlite3_int64 raw = sqlite3_column_int64(stmt, col);
    if (raw < 0 || raw > max_val) {
        return false;
    }

    out_val = static_cast<UintT>(raw);
    return true;
}

template <typename UintT>
static bool column_optional_bounded(sqlite3_stmt* stmt, int col, sqlite3_int64 max_val,
                                    std::optional<UintT>& out_val) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
        out_val = std::nullopt;
        return true;
    }

    UintT parsed = 0;
    if (!column_nonneg_bounded(stmt, col, max_val, parsed)) {
        return false;
    }

    out_val = parsed;
    return true;
}

static bool column_nonneg_u64(sqlite3_stmt* stmt, int col, uint64_t& out_val) {
    return column_nonneg_bounded(stmt, col, INT64_MAX, out_val);
}

static bool column_nonneg_u32(sqlite3_stmt* stmt, int col, uint32_t& out_val) {
    return column_nonneg_bounded(stmt, col, static_cast<sqlite3_int64>(UINT32_MAX), out_val);
}

static bool column_optional_nonneg_u64(sqlite3_stmt* stmt, int col, std::optional<uint64_t>& out_val) {
    return column_optional_bounded(stmt, col, INT64_MAX, out_val);
}

static bool column_optional_nonneg_u32(sqlite3_stmt* stmt, int col, std::optional<uint32_t>& out_val) {
    return column_optional_bounded(stmt, col, static_cast<sqlite3_int64>(UINT32_MAX), out_val);
}

static bool column_optional_u8(sqlite3_stmt* stmt, int col, std::optional<uint8_t>& out_val) {
    return column_optional_bounded(stmt, col, 255, out_val);
}

static bool column_optional_positive_pid(sqlite3_stmt* stmt, int col, std::optional<uint32_t>& out_pid) {
    if (!column_optional_nonneg_u32(stmt, col, out_pid)) {
        return false;
    }

    if (out_pid.has_value() && *out_pid == 0) {
        out_pid.reset();
    }

    return true;
}

static sqlite_stmt_ptr prepare_query(sqlite3* db, const char* sql, std::string& error) {
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        error = std::string("SQLite query failed: ") + sqlite3_errmsg(db);
    }

    return sqlite_stmt_ptr(raw_stmt, sqlite3_finalize);
}

static bool sqlite_table_exists(sqlite3* db, const char* table_name) {
    sqlite3_stmt* raw_stmt = nullptr;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite_stmt_ptr stmt(raw_stmt, sqlite3_finalize);
    sqlite3_bind_text(stmt.get(), 1, table_name, -1, SQLITE_STATIC);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

static bool fail_load(ktrace_profile_model& model, std::string& error, std::string message) {
    model = {};
    error = std::move(message);
    return false;
}

static bool verify_step_done(sqlite3* db, int step_rc, ktrace_profile_model& model, std::string& error) {
    if (step_rc == SQLITE_DONE) {
        return true;
    }

    return fail_load(model, error, std::string("SQLite step error: ") + sqlite3_errmsg(db));
}

static uint64_t interval_overlap(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
    uint64_t lo = std::max(a_start, b_start);
    uint64_t hi = std::min(a_end, b_end);
    return hi > lo ? (hi - lo) : 0;
}

static bool is_negated_syscall_errno(int64_t result) {
    return result >= NEGATED_ERRNO_MIN && result <= NEGATED_ERRNO_MAX;
}

static uint64_t sum_overlap_with_sorted(const std::vector<time_interval>& intervals,
                                        uint64_t query_start, uint64_t query_end) {
    if (query_end <= query_start || intervals.empty()) {
        return 0;
    }

    auto iter = std::lower_bound(intervals.begin(), intervals.end(), query_start,
                                 [](const time_interval& candidate, uint64_t target) {
                                     return candidate.end_ns <= target;
                                 });
    uint64_t total = 0;
    for (; iter != intervals.end() && iter->start_ns < query_end; ++iter) {
        total = saturating_add_u64(total, interval_overlap(iter->start_ns, iter->end_ns, query_start, query_end));
    }

    return total;
}

static uint64_t union_covered_duration(std::vector<time_interval> intervals) {
    if (intervals.empty()) {
        return 0;
    }

    std::sort(intervals.begin(), intervals.end(), [](const time_interval& lhs, const time_interval& rhs) {
        return lhs.start_ns != rhs.start_ns ? (lhs.start_ns < rhs.start_ns) : (lhs.end_ns < rhs.end_ns);
    });

    uint64_t covered = 0;
    uint64_t cur_start = intervals[0].start_ns;
    uint64_t cur_end = intervals[0].end_ns;
    for (size_t idx = 1; idx < intervals.size(); idx++) {
        if (intervals[idx].start_ns <= cur_end) {
            cur_end = std::max(cur_end, intervals[idx].end_ns);
        } else {
            covered = saturating_add_u64(covered, cur_end - cur_start);
            cur_start = intervals[idx].start_ns;
            cur_end = intervals[idx].end_ns;
        }
    }

    return saturating_add_u64(covered, cur_end - cur_start);
}

bool is_ktrace_capture(std::span<const uint8_t> header_bytes) {
    return header_bytes.size() >= KTRACE_CAPTURE_MAGIC.size() &&
           std::memcmp(header_bytes.data(), KTRACE_CAPTURE_MAGIC.data(), KTRACE_CAPTURE_MAGIC.size()) == 0;
}

bool is_sqlite_database(std::span<const uint8_t> header_bytes) {
    return header_bytes.size() >= SQLITE_HEADER_PREFIX_BYTES &&
           std::memcmp(header_bytes.data(), SQLITE_HEADER_MAGIC, SQLITE_HEADER_PREFIX_BYTES) == 0;
}

static bool check_raw_ktrace_prefix(const std::string& db_path, std::string& error) {
    FILE* file = std::fopen(db_path.c_str(), "rb");
    if (file == nullptr) {
        return true;
    }

    uint8_t prefix[SQLITE_HEADER_PREFIX_BYTES] = {};
    size_t nread = std::fread(prefix, 1, sizeof(prefix), file);
    std::fclose(file);

    if (is_ktrace_capture(std::span<const uint8_t>(prefix, nread))) {
        error = "raw ktrace capture must be decoded with ktrace-decode first";
        return false;
    }

    return true;
}

template <typename RowFn>
static bool scan_optional_table(sqlite3* db, const char* table, const char* sql,
                                ktrace_profile_model& model, std::string& error, RowFn&& on_row) {
    if (!sqlite_table_exists(db, table)) {
        return true;
    }

    sqlite_stmt_ptr stmt = prepare_query(db, sql, error);
    if (!stmt) {
        return fail_load(model, error, error);
    }

    size_t row_count = 0;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        if (++row_count > KTRACE_MAX_ROWS_PER_TABLE) {
            return fail_load(model, error, std::string("ktrace ") + table + " table exceeds row limit");
        }

        if (!on_row(stmt.get())) {
            return false;
        }
    }

    return verify_step_done(db, rc, model, error);
}

bool load_ktrace_database(const std::string& db_path, ktrace_profile_model& model, std::string& error) {
    model = {};
    error.clear();
    if (!check_raw_ktrace_prefix(db_path, error)) {
        return false;
    }

    sqlite3* raw_db = nullptr;
    int open_rc = sqlite3_open_v2(db_path.c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr);
    sqlite_db_ptr db(raw_db, sqlite3_close_v2);
    if (open_rc != SQLITE_OK || !db) {
        return fail_load(model, error, std::string("cannot open ktrace database: ") +
                                           (db ? sqlite3_errmsg(db.get()) : sqlite3_errstr(open_rc)));
    }

    std::unordered_map<int64_t, std::string> names_by_id;
    if (!scan_optional_table(db.get(), "names", "SELECT name_id, name FROM names", model, error,
                             [&](sqlite3_stmt* stmt) {
                                 names_by_id[sqlite3_column_int64(stmt, 0)] = column_string(stmt, 1);
                                 return true;
                             })) {
        return false;
    }

    sqlite_stmt_ptr session_stmt = prepare_query(
        db.get(),
        "SELECT format_version, arch, elf_machine, cpu_count, boot_unix_ns, start_ns, stop_ns, "
        "event_mask, records, records_before_start, lost_records, file_bytes FROM session",
        error);
    if (!session_stmt) {
        return fail_load(model, error, error);
    }

    int session_rc = sqlite3_step(session_stmt.get());
    if (session_rc != SQLITE_ROW) {
        return fail_load(model, error, session_rc == SQLITE_DONE
                                           ? std::string("ktrace database session table is empty")
                                           : (std::string("SQLite session error: ") + sqlite3_errmsg(db.get())));
    }

    uint32_t elf_mach = 0;
    if (!column_nonneg_u32(session_stmt.get(), 0, model.session.format_version) ||
        !column_nonneg_u32(session_stmt.get(), 2, elf_mach) || elf_mach > UINT16_MAX ||
        !column_nonneg_u32(session_stmt.get(), 3, model.session.cpu_count) ||
        !column_optional_nonneg_u64(session_stmt.get(), 4, model.session.boot_unix_ns) ||
        !column_nonneg_u64(session_stmt.get(), 5, model.session.start_ns) ||
        !column_optional_nonneg_u64(session_stmt.get(), 6, model.session.stop_ns) ||
        !column_nonneg_u64(session_stmt.get(), 8, model.session.records) ||
        !column_nonneg_u64(session_stmt.get(), 9, model.session.records_before_start) ||
        !column_nonneg_u64(session_stmt.get(), 10, model.session.lost_records) ||
        !column_nonneg_u64(session_stmt.get(), 11, model.session.file_bytes)) {
        return fail_load(model, error, "ktrace session table holds negative integer values");
    }

    model.session.arch = column_string(session_stmt.get(), 1);
    model.session.elf_machine = static_cast<uint16_t>(elf_mach);
    model.session.event_mask = column_raw_bitcast_u64(session_stmt.get(), 7);

    if (!scan_optional_table(db.get(), "health", "SELECT name, passed, detail FROM health ORDER BY rowid",
                             model, error, [&](sqlite3_stmt* stmt) {
                                 model.health_checks.push_back({
                                     column_string(stmt, 0), sqlite3_column_int(stmt, 1) != 0, column_string(stmt, 2),
                                 });
                                 return true;
                             })) {
        return false;
    }

    if (!scan_optional_table(db.get(), "cpus",
                             "SELECT cpu, records, records_before_start, lost_records FROM cpus ORDER BY cpu",
                             model, error, [&](sqlite3_stmt* stmt) {
                                 ktrace_cpu_counter c;
                                 if (!column_nonneg_u32(stmt, 0, c.cpu) || !column_nonneg_u64(stmt, 1, c.records) ||
                                     !column_nonneg_u64(stmt, 2, c.records_before_start) ||
                                     !column_nonneg_u64(stmt, 3, c.lost_records)) {
                                     return fail_load(model, error, "ktrace cpus table holds negative values");
                                 }

                                 model.cpus.push_back(c);
                                 return true;
                             })) {
        return false;
    }

    std::unordered_map<uint32_t, std::vector<ktrace_thread_name_span>> thread_names_by_tid;
    if (!scan_optional_table(db.get(), "thread_names",
                             "SELECT tid, name_id, first_ns, last_ns FROM thread_names ORDER BY tid, first_ns",
                             model, error, [&](sqlite3_stmt* stmt) {
                                 uint32_t tid = 0;
                                 uint64_t first_ns = 0;
                                 uint64_t last_ns = 0;
                                 if (!column_nonneg_u32(stmt, 0, tid) || !column_nonneg_u64(stmt, 2, first_ns) ||
                                     !column_nonneg_u64(stmt, 3, last_ns)) {
                                     return fail_load(model, error, "ktrace thread_names holds negative values");
                                 }

                                 auto found = names_by_id.find(sqlite3_column_int64(stmt, 1));
                                 thread_names_by_tid[tid].push_back({
                                     found != names_by_id.end() ? found->second : std::string{}, first_ns, last_ns,
                                 });
                                 return true;
                             })) {
        return false;
    }

    if (!scan_optional_table(db.get(), "threads",
                             "SELECT tid, kind, pid, first_ns, last_ns, exit_ns FROM threads ORDER BY tid",
                             model, error, [&](sqlite3_stmt* stmt) {
                                 ktrace_thread t;
                                 if (!column_nonneg_u32(stmt, 0, t.tid) || !column_optional_u8(stmt, 1, t.kind) ||
                                     !column_optional_positive_pid(stmt, 2, t.pid) ||
                                     !column_nonneg_u64(stmt, 3, t.first_ns) ||
                                     !column_nonneg_u64(stmt, 4, t.last_ns) ||
                                     !column_optional_nonneg_u64(stmt, 5, t.exit_ns)) {
                                     return fail_load(model, error, "ktrace threads holds invalid values");
                                 }

                                 if (auto it = thread_names_by_tid.find(t.tid); it != thread_names_by_tid.end()) {
                                     t.names = std::move(it->second);
                                 }

                                 model.threads.push_back(std::move(t));
                                 return true;
                             })) {
        return false;
    }

    if (!scan_optional_table(db.get(), "processes",
                             "SELECT pid, leader_name_id, threads, first_ns, last_ns FROM processes ORDER BY pid",
                             model, error, [&](sqlite3_stmt* stmt) {
                                 ktrace_process proc;
                                 if (!column_nonneg_u32(stmt, 0, proc.pid) ||
                                     !column_nonneg_u64(stmt, 2, proc.thread_count) ||
                                     !column_nonneg_u64(stmt, 3, proc.first_ns) ||
                                     !column_nonneg_u64(stmt, 4, proc.last_ns)) {
                                     return fail_load(model, error, "ktrace processes holds negative values");
                                 }

                                 if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
                                     if (auto it = names_by_id.find(sqlite3_column_int64(stmt, 1));
                                         it != names_by_id.end()) {
                                         proc.leader_name = it->second;
                                     }
                                 }

                                 model.processes.push_back(std::move(proc));
                                 return true;
                             })) {
        return false;
    }

    if (!scan_optional_table(db.get(), "cpu_slices",
                             "SELECT cpu, tid, start_ns, end_ns FROM cpu_slices ORDER BY start_ns, cpu",
                             model, error, [&](sqlite3_stmt* stmt) {
                                 ktrace_cpu_slice s;
                                 if (!column_nonneg_u32(stmt, 0, s.cpu) || !column_nonneg_u32(stmt, 1, s.tid) ||
                                     !column_nonneg_u64(stmt, 2, s.start_ns) ||
                                     !column_optional_nonneg_u64(stmt, 3, s.end_ns)) {
                                     return fail_load(model, error, "ktrace cpu_slices holds negative values");
                                 }

                                 model.cpu_slices.push_back(s);
                                 return true;
                             })) {
        return false;
    }

    if (!scan_optional_table(
            db.get(), "thread_states",
            "SELECT tid, state, start_ns, end_ns, cpu, waker_tid, switch_reason, syscall_number, in_page_fault "
            "FROM thread_states ORDER BY tid, start_ns",
            model, error, [&](sqlite3_stmt* stmt) {
                std::string name = column_string(stmt, 1);
                if (name != "running" && name != "runnable" && name != "sleeping") {
                    return fail_load(model, error, "unknown thread_states state: " + name);
                }

                ktrace_thread_state s;
                s.state = name == "runnable"
                              ? thread_state_kind::RUNNABLE
                              : (name == "sleeping" ? thread_state_kind::SLEEPING : thread_state_kind::RUNNING);
                if (!column_nonneg_u32(stmt, 0, s.tid) || !column_nonneg_u64(stmt, 2, s.start_ns) ||
                    !column_optional_nonneg_u64(stmt, 3, s.end_ns) || !column_optional_nonneg_u32(stmt, 4, s.cpu) ||
                    !column_optional_nonneg_u32(stmt, 5, s.waker_tid) || !column_optional_u8(stmt, 6, s.switch_reason) ||
                    !column_optional_nonneg_u32(stmt, 7, s.syscall_number)) {
                    return fail_load(model, error, "ktrace thread_states holds invalid values");
                }

                s.in_page_fault = sqlite3_column_type(stmt, 8) != SQLITE_NULL && sqlite3_column_int(stmt, 8) != 0;
                model.thread_states.push_back(s);
                return true;
            })) {
        return false;
    }

    if (!scan_optional_table(
            db.get(), "syscall",
            "SELECT ts_ns, cpu, duration_ns, result, number, tid, pid FROM syscall ORDER BY ts_ns, rowid",
            model, error, [&](sqlite3_stmt* stmt) {
                ktrace_syscall_event c;
                if (!column_nonneg_u64(stmt, 0, c.end_ns) || !column_nonneg_u32(stmt, 1, c.cpu) ||
                    !column_nonneg_u64(stmt, 2, c.duration_ns) || !column_nonneg_u32(stmt, 4, c.number) ||
                    !column_nonneg_u32(stmt, 5, c.tid) || !column_nonneg_u32(stmt, 6, c.pid)) {
                    return fail_load(model, error, "ktrace syscall holds negative values");
                }

                c.start_ns = c.end_ns >= c.duration_ns ? (c.end_ns - c.duration_ns) : 0;
                c.result = sqlite3_column_int64(stmt, 3);
                model.syscalls.push_back(c);
                return true;
            })) {
        return false;
    }

    if (!scan_optional_table(
            db.get(), "page_fault",
            "SELECT ts_ns, cpu, address, flags, duration_ns, result, tid FROM page_fault ORDER BY ts_ns, rowid",
            model, error, [&](sqlite3_stmt* stmt) {
                ktrace_page_fault_event f;
                if (!column_nonneg_u64(stmt, 0, f.end_ns) || !column_nonneg_u32(stmt, 1, f.cpu) ||
                    !column_nonneg_u64(stmt, 3, f.flags) || !column_nonneg_u64(stmt, 4, f.duration_ns) ||
                    !column_nonneg_u32(stmt, 6, f.tid)) {
                    return fail_load(model, error, "ktrace page_fault holds negative values");
                }

                f.start_ns = f.end_ns >= f.duration_ns ? (f.end_ns - f.duration_ns) : 0;
                f.address = column_raw_bitcast_u64(stmt, 2);
                f.result = static_cast<int32_t>(sqlite3_column_int(stmt, 5));
                model.page_faults.push_back(f);
                return true;
            })) {
        return false;
    }

    finalize_ktrace_model(model);
    return true;
}

static const char* lookup_sorted_numbered_name(std::span<const numbered_name> table, uint32_t code) {
    auto iter = std::lower_bound(table.begin(), table.end(), code,
                                 [](const numbered_name& entry, uint32_t target) { return entry.code < target; });
    return (iter != table.end() && iter->code == code) ? iter->name : nullptr;
}

static bool is_x86_64_session(const ktrace_session& session) {
    return session.arch == "x86_64" || session.elf_machine == EM_X86_64_CODE;
}

static bool is_aarch64_session(const ktrace_session& session) {
    return session.arch == "aarch64" || session.elf_machine == EM_AARCH64_CODE;
}

std::string resolve_syscall_name(const ktrace_profile_model& model, uint32_t syscall_number) {
    auto custom = model.custom_syscall_names.find(syscall_number);
    if (custom != model.custom_syscall_names.end() && !custom->second.empty()) {
        return custom->second;
    }

    if (const char* stellux_name = lookup_sorted_numbered_name(STELLUX_SYSCALL_TABLE, syscall_number)) {
        return stellux_name;
    }

    if (is_x86_64_session(model.session)) {
        if (const char* arch_name = lookup_sorted_numbered_name(X86_64_SYSCALL_TABLE, syscall_number)) {
            return arch_name;
        }
    } else if (is_aarch64_session(model.session)) {
        if (const char* arch_name = lookup_sorted_numbered_name(AARCH64_SYSCALL_TABLE, syscall_number)) {
            return arch_name;
        }
    }

    return "sys_" + std::to_string(syscall_number);
}

std::string resolve_errno_name(const ktrace_profile_model& model, int32_t errno_code) {
    if (errno_code <= 0) {
        return {};
    }

    auto custom = model.custom_errno_names.find(errno_code);
    if (custom != model.custom_errno_names.end() && !custom->second.empty()) {
        return custom->second;
    }

    auto iter = std::lower_bound(std::begin(STELLUX_ERRNO_TABLE), std::end(STELLUX_ERRNO_TABLE), errno_code,
                                 [](const errno_entry& entry, int32_t target) { return entry.code < target; });
    if (iter != std::end(STELLUX_ERRNO_TABLE) && iter->code == errno_code) {
        return iter->name;
    }

    return "ERRNO_" + std::to_string(errno_code);
}

static std::string resolve_thread_label(const ktrace_thread* thread, uint32_t tid, uint64_t ts_ns) {
    if (thread != nullptr) {
        auto iter = std::lower_bound(thread->names.begin(), thread->names.end(), ts_ns,
                                     [](const ktrace_thread_name_span& span, uint64_t target) {
                                         return span.last_ns < target;
                                     });
        if (iter != thread->names.end() && ts_ns >= iter->first_ns && ts_ns <= iter->last_ns) {
            return iter->name;
        }

        if (!thread->latest_name.empty()) {
            return thread->latest_name;
        }
    }

    return "tid " + std::to_string(tid);
}

std::string thread_name_at(const ktrace_profile_model& model, uint32_t tid, uint64_t ts_ns) {
    auto iter = std::lower_bound(model.threads.begin(), model.threads.end(), tid,
                                 [](const ktrace_thread& thread, uint32_t target) { return thread.tid < target; });
    const ktrace_thread* found = (iter != model.threads.end() && iter->tid == tid) ? &*iter : nullptr;
    return resolve_thread_label(found, tid, ts_ns);
}

std::string format_duration_ns(uint64_t ns) {
    char buf[64] = {};
    if (ns >= NS_IN_SEC) {
        std::snprintf(buf, sizeof(buf), "%.2f s", static_cast<double>(ns) / static_cast<double>(NS_IN_SEC));
    } else if (ns >= NS_IN_MSEC) {
        std::snprintf(buf, sizeof(buf), "%.2f ms", static_cast<double>(ns) / static_cast<double>(NS_IN_MSEC));
    } else if (ns >= NS_IN_USEC) {
        std::snprintf(buf, sizeof(buf), "%.1f us", static_cast<double>(ns) / static_cast<double>(NS_IN_USEC));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu ns", static_cast<unsigned long long>(ns));
    }

    return buf;
}

void finalize_ktrace_model(ktrace_profile_model& model) {
    model.all_health_checks_passed =
        !model.health_checks.empty() &&
        std::all_of(model.health_checks.begin(), model.health_checks.end(),
                    [](const ktrace_health_status& check) { return check.passed; });

    for (ktrace_process& proc : model.processes) {
        proc.is_recorder = (proc.leader_name == RECORDER_LEADER_NAME);
    }

    std::sort(model.threads.begin(), model.threads.end(),
              [](const ktrace_thread& lhs, const ktrace_thread& rhs) { return lhs.tid < rhs.tid; });

    std::unordered_map<uint32_t, uint8_t> kind_by_tid;
    for (ktrace_thread& thread : model.threads) {
        if (thread.pid.has_value() && *thread.pid == 0) {
            thread.pid.reset();
        }

        std::sort(thread.names.begin(), thread.names.end(),
                  [](const ktrace_thread_name_span& a, const ktrace_thread_name_span& b) {
                      return a.first_ns < b.first_ns;
                  });
        if (thread.latest_name.empty() && !thread.names.empty()) {
            const ktrace_thread_name_span* latest = &thread.names.front();
            for (const ktrace_thread_name_span& span : thread.names) {
                if (span.last_ns >= latest->last_ns) {
                    latest = &span;
                }
            }

            thread.latest_name = latest->name;
        }

        if (thread.kind.has_value()) {
            kind_by_tid[thread.tid] = *thread.kind;
        }
    }

    std::sort(model.cpu_slices.begin(), model.cpu_slices.end(),
              [](const ktrace_cpu_slice& a, const ktrace_cpu_slice& b) {
                  return a.start_ns != b.start_ns ? (a.start_ns < b.start_ns) : (a.cpu < b.cpu);
              });
    for (ktrace_cpu_slice& slice : model.cpu_slices) {
        if (auto it = kind_by_tid.find(slice.tid); it != kind_by_tid.end()) {
            slice.is_idle = (it->second == KTRACE_TASK_IDLE);
        }
    }

    std::sort(model.thread_states.begin(), model.thread_states.end(),
              [](const ktrace_thread_state& a, const ktrace_thread_state& b) {
                  return a.tid != b.tid ? (a.tid < b.tid) : (a.start_ns < b.start_ns);
              });
    std::sort(model.syscalls.begin(), model.syscalls.end(),
              [](const ktrace_syscall_event& a, const ktrace_syscall_event& b) {
                  return a.start_ns != b.start_ns ? (a.start_ns < b.start_ns) : (a.end_ns < b.end_ns);
              });
    std::sort(model.page_faults.begin(), model.page_faults.end(),
              [](const ktrace_page_fault_event& a, const ktrace_page_fault_event& b) {
                  return a.start_ns != b.start_ns ? (a.start_ns < b.start_ns) : (a.end_ns < b.end_ns);
              });

    std::unordered_map<uint32_t, std::vector<time_interval>> sleeping_by_tid;
    for (const ktrace_thread_state& s : model.thread_states) {
        if (s.state == thread_state_kind::SLEEPING && s.end_ns.has_value() && *s.end_ns > s.start_ns) {
            sleeping_by_tid[s.tid].push_back({ s.start_ns, *s.end_ns });
        }
    }

    for (ktrace_syscall_event& call : model.syscalls) {
        if (call.duration_ns == 0 && call.end_ns > call.start_ns) {
            call.duration_ns = call.end_ns - call.start_ns;
        }

        call.is_error = is_negated_syscall_errno(call.result);
        call.errno_code = call.is_error ? static_cast<int32_t>(-call.result) : 0;
        auto it = sleeping_by_tid.find(call.tid);
        call.sleep_ns = it != sleeping_by_tid.end() ? sum_overlap_with_sorted(it->second, call.start_ns, call.end_ns) : 0;
    }

    if (model.session.stop_ns.has_value()) {
        model.session.effective_end_ns = *model.session.stop_ns;
        return;
    }

    uint64_t max_ts = model.session.start_ns;
    for (const auto& s : model.cpu_slices) {
        max_ts = std::max(max_ts, s.end_ns.value_or(s.start_ns));
    }

    for (const auto& s : model.thread_states) {
        max_ts = std::max(max_ts, s.end_ns.value_or(s.start_ns));
    }

    for (const auto& c : model.syscalls) {
        max_ts = std::max(max_ts, c.end_ns);
    }

    for (const auto& f : model.page_faults) {
        max_ts = std::max(max_ts, f.end_ns);
    }

    model.session.effective_end_ns = max_ts;
}

static bool matches_thread_filter(uint32_t tid, std::optional<uint32_t> fallback_pid,
                                  const ktrace_window_filter& filter,
                                  const std::unordered_map<uint32_t, const ktrace_thread*>& thread_by_tid,
                                  const std::set<uint32_t>& recorder_pids) {
    if (filter.tid.has_value() && tid != *filter.tid) {
        return false;
    }

    std::optional<uint32_t> resolved_pid = fallback_pid;
    auto found = thread_by_tid.find(tid);
    if (found != thread_by_tid.end() && found->second->pid.has_value() && *found->second->pid > 0) {
        resolved_pid = found->second->pid;
    }

    if (filter.pid.has_value() && (!resolved_pid.has_value() || *resolved_pid != *filter.pid)) {
        return false;
    }

    if (filter.exclude_recorder) {
        if (resolved_pid.has_value() && recorder_pids.count(*resolved_pid) > 0) {
            return false;
        }

        if (found != thread_by_tid.end() && found->second->latest_name == RECORDER_LEADER_NAME) {
            return false;
        }
    }

    return true;
}

static size_t ensure_tree_child(std::vector<flame_tree_node>& nodes, size_t parent_idx,
                                const std::string& name, uint32_t depth) {
    auto existing = nodes[parent_idx].child_by_name.find(name);
    if (existing != nodes[parent_idx].child_by_name.end()) {
        return existing->second;
    }

    size_t child_idx = nodes.size();
    nodes[parent_idx].child_by_name[name] = child_idx;
    nodes[parent_idx].children.push_back(child_idx);
    nodes.push_back({ name, depth, 0, 0, 0, {}, {} });
    return child_idx;
}

static bool is_enclosed_in_sorted(const std::vector<time_interval>& outer_spans,
                                  uint64_t inner_start, uint64_t inner_end) {
    auto iter = std::lower_bound(outer_spans.begin(), outer_spans.end(), inner_end,
                                 [](const time_interval& span, uint64_t end_ts) {
                                     return span.end_ns < end_ts;
                                 });
    for (; iter != outer_spans.end() && iter->start_ns <= inner_start; ++iter) {
        if (iter->start_ns <= inner_start && inner_end <= iter->end_ns) {
            return true;
        }
    }

    return false;
}

static void append_overlaps_with_sorted(const std::vector<time_interval>& running_spans,
                                        uint64_t query_start, uint64_t query_end,
                                        uint64_t& out_overlap_sum,
                                        std::vector<time_interval>& out_overlaps) {
    out_overlap_sum = 0;
    auto iter = std::lower_bound(running_spans.begin(), running_spans.end(), query_start,
                                 [](const time_interval& candidate, uint64_t target) {
                                     return candidate.end_ns <= target;
                                 });
    for (; iter != running_spans.end() && iter->start_ns < query_end; ++iter) {
        uint64_t lo = std::max(iter->start_ns, query_start);
        uint64_t hi = std::min(iter->end_ns, query_end);
        if (hi > lo) {
            out_overlap_sum = saturating_add_u64(out_overlap_sum, hi - lo);
            out_overlaps.push_back({ lo, hi });
        }
    }
}

static void populate_tree_from_kernel_activity(const ktrace_profile_model& model,
                                               uint64_t win_start, uint64_t win_end,
                                               const ktrace_window_filter& filter,
                                               const std::unordered_map<uint32_t, const ktrace_thread*>& thread_by_tid,
                                               const std::set<uint32_t>& recorder_pids,
                                               std::vector<flame_tree_node>& nodes) {
    std::unordered_map<uint32_t, std::vector<time_interval>> running_by_tid;
    std::unordered_map<uint32_t, uint64_t> running_total_by_tid;
    std::unordered_map<uint32_t, size_t> root_node_by_tid;

    for (const ktrace_cpu_slice& slice : model.cpu_slices) {
        if (slice.is_idle || !slice.end_ns.has_value() || *slice.end_ns <= slice.start_ns) {
            continue;
        }

        if (!matches_thread_filter(slice.tid, std::nullopt, filter, thread_by_tid, recorder_pids)) {
            continue;
        }

        uint64_t c0 = std::max(slice.start_ns, win_start);
        uint64_t c1 = std::min(*slice.end_ns, win_end);
        if (c1 <= c0) {
            continue;
        }

        auto& vec = running_by_tid[slice.tid];
        vec.push_back({ c0, c1 });
        running_total_by_tid[slice.tid] = saturating_add_u64(running_total_by_tid[slice.tid], c1 - c0);
        if (root_node_by_tid.find(slice.tid) == root_node_by_tid.end()) {
            auto thread_it = thread_by_tid.find(slice.tid);
            const ktrace_thread* thread_ptr = thread_it != thread_by_tid.end() ? thread_it->second : nullptr;
            std::string label = resolve_thread_label(thread_ptr, slice.tid, c0);
            root_node_by_tid[slice.tid] = ensure_tree_child(nodes, 0, label, 0);
        }
    }

    for (auto& [tid, intervals] : running_by_tid) {
        (void)tid;
        std::sort(intervals.begin(), intervals.end(),
                  [](const time_interval& lhs, const time_interval& rhs) { return lhs.start_ns < rhs.start_ns; });
    }

    std::unordered_map<uint32_t, std::vector<time_interval>> syscalls_by_tid;
    std::unordered_map<uint32_t, std::vector<time_interval>> covered_children_by_tid;

    auto record_child = [&](uint32_t tid, const std::vector<time_interval>& run_spans,
                            uint64_t ev_start, uint64_t ev_end, const std::string& label) {
        uint64_t c0 = std::max(ev_start, win_start);
        uint64_t c1 = std::min(ev_end, win_end);
        uint64_t on_cpu_ns = 0;
        if (c1 > c0) {
            append_overlaps_with_sorted(run_spans, c0, c1, on_cpu_ns, covered_children_by_tid[tid]);
        }

        if (on_cpu_ns > 0) {
            size_t child_idx = ensure_tree_child(nodes, root_node_by_tid[tid], label, 1);
            nodes[child_idx].total_ns = saturating_add_u64(nodes[child_idx].total_ns, on_cpu_ns);
            nodes[child_idx].self_ns = saturating_add_u64(nodes[child_idx].self_ns, on_cpu_ns);
            nodes[child_idx].call_count += 1;
        }
    };

    for (const ktrace_syscall_event& call : model.syscalls) {
        auto run_it = running_by_tid.find(call.tid);
        if (run_it != running_by_tid.end() && call.end_ns > call.start_ns) {
            syscalls_by_tid[call.tid].push_back({ call.start_ns, call.end_ns });
            record_child(call.tid, run_it->second, call.start_ns, call.end_ns,
                         resolve_syscall_name(model, call.number));
        }
    }

    for (auto& [tid, intervals] : syscalls_by_tid) {
        (void)tid;
        std::sort(intervals.begin(), intervals.end(), [](const time_interval& lhs, const time_interval& rhs) {
            return lhs.end_ns != rhs.end_ns ? (lhs.end_ns < rhs.end_ns) : (lhs.start_ns < rhs.start_ns);
        });
    }

    for (const ktrace_page_fault_event& fault : model.page_faults) {
        auto run_it = running_by_tid.find(fault.tid);
        if (run_it == running_by_tid.end() || fault.end_ns <= fault.start_ns) {
            continue;
        }

        auto sys_it = syscalls_by_tid.find(fault.tid);
        if (sys_it == syscalls_by_tid.end() || !is_enclosed_in_sorted(sys_it->second, fault.start_ns, fault.end_ns)) {
            record_child(fault.tid, run_it->second, fault.start_ns, fault.end_ns, "page_fault");
        }
    }

    for (const auto& [tid, root_idx] : root_node_by_tid) {
        uint64_t total_run = running_total_by_tid[tid];
        uint64_t covered = union_covered_duration(covered_children_by_tid[tid]);
        nodes[root_idx].total_ns = saturating_add_u64(nodes[root_idx].total_ns, total_run);
        nodes[root_idx].self_ns = saturating_add_u64(nodes[root_idx].self_ns,
                                                     total_run >= covered ? (total_run - covered) : 0);
        nodes[root_idx].call_count += running_by_tid[tid].size();
    }
}

static void collect_hotspots_from_tree(const std::vector<flame_tree_node>& nodes, size_t node_idx,
                                       std::set<std::string>& ancestors_on_path,
                                       std::map<std::string, ktrace_hotspot>& hotspots_by_name) {
    if (node_idx != 0) {
        const flame_tree_node& node = nodes[node_idx];
        ktrace_hotspot& agg = hotspots_by_name[node.name];
        agg.name = node.name;
        agg.self_ns = saturating_add_u64(agg.self_ns, node.self_ns);
        agg.call_count = saturating_add_u64(agg.call_count, node.call_count);

        bool inserted = ancestors_on_path.insert(node.name).second;
        if (inserted) {
            agg.total_ns = saturating_add_u64(agg.total_ns, node.total_ns);
        }

        for (size_t child_idx : node.children) {
            collect_hotspots_from_tree(nodes, child_idx, ancestors_on_path, hotspots_by_name);
        }

        if (inserted) {
            ancestors_on_path.erase(node.name);
        }

        return;
    }

    for (size_t child_idx : nodes[0].children) {
        collect_hotspots_from_tree(nodes, child_idx, ancestors_on_path, hotspots_by_name);
    }
}

ktrace_window_view query_ktrace_window(const ktrace_profile_model& model, const ktrace_window_filter& filter) {
    ktrace_window_view view;
    view.start_ns = filter.start_ns;
    view.end_ns = filter.end_ns;
    if (filter.end_ns <= filter.start_ns) {
        return view;
    }

    view.window_ns = filter.end_ns - filter.start_ns;

    std::unordered_map<uint32_t, const ktrace_thread*> thread_by_tid;
    for (const ktrace_thread& thread : model.threads) {
        thread_by_tid[thread.tid] = &thread;
    }

    std::set<uint32_t> recorder_pids;
    for (const ktrace_process& proc : model.processes) {
        if (proc.is_recorder) {
            recorder_pids.insert(proc.pid);
        }
    }

    bool scoped_to_task = filter.tid.has_value() || filter.pid.has_value();
    for (const ktrace_cpu_slice& slice : model.cpu_slices) {
        if ((slice.is_idle && scoped_to_task) ||
            (!slice.is_idle && !matches_thread_filter(slice.tid, std::nullopt, filter, thread_by_tid, recorder_pids))) {
            continue;
        }

        if (!slice.end_ns.has_value()) {
            view.indeterminate_slice_count += (slice.start_ns < filter.end_ns) ? 1 : 0;
            continue;
        }

        uint64_t overlap = interval_overlap(slice.start_ns, *slice.end_ns, filter.start_ns, filter.end_ns);
        uint64_t& target = slice.is_idle ? view.idle_cpu_ns : view.busy_cpu_ns;
        target = saturating_add_u64(target, overlap);
    }

    std::unordered_map<uint32_t, std::vector<time_interval>> clipped_sleeping_by_tid;
    for (const ktrace_thread_state& state : model.thread_states) {
        if (!matches_thread_filter(state.tid, std::nullopt, filter, thread_by_tid, recorder_pids)) {
            continue;
        }

        if (!state.end_ns.has_value()) {
            view.indeterminate_state_count += (state.start_ns < filter.end_ns) ? 1 : 0;
            continue;
        }

        uint64_t c0 = std::max(state.start_ns, filter.start_ns);
        uint64_t c1 = std::min(*state.end_ns, filter.end_ns);
        if (c1 <= c0) {
            continue;
        }

        uint64_t dur = c1 - c0;
        if (state.state == thread_state_kind::RUNNING) {
            view.running_ns = saturating_add_u64(view.running_ns, dur);
        } else if (state.state == thread_state_kind::RUNNABLE) {
            view.runnable_ns = saturating_add_u64(view.runnable_ns, dur);
        } else {
            view.sleeping_ns = saturating_add_u64(view.sleeping_ns, dur);
            clipped_sleeping_by_tid[state.tid].push_back({ c0, c1 });
        }
    }

    std::map<uint32_t, ktrace_syscall_summary> syscalls_by_number;
    for (const ktrace_syscall_event& call : model.syscalls) {
        if (!matches_thread_filter(call.tid, call.pid != 0 ? std::optional<uint32_t>(call.pid) : std::nullopt,
                                   filter, thread_by_tid, recorder_pids)) {
            continue;
        }

        bool zero_dur_in_window = (call.end_ns == call.start_ns) &&
                                  (call.start_ns >= filter.start_ns && call.start_ns < filter.end_ns);
        uint64_t c0 = std::max(call.start_ns, filter.start_ns);
        uint64_t c1 = std::min(call.end_ns, filter.end_ns);
        if (c1 <= c0 && !zero_dur_in_window) {
            continue;
        }

        uint64_t clipped_wall = c1 > c0 ? (c1 - c0) : 0;
        auto sleep_it = clipped_sleeping_by_tid.find(call.tid);
        uint64_t clipped_sleep = (sleep_it != clipped_sleeping_by_tid.end() && c1 > c0)
                                     ? sum_overlap_with_sorted(sleep_it->second, c0, c1)
                                     : 0;

        ktrace_syscall_summary& summary = syscalls_by_number[call.number];
        if (summary.name.empty()) {
            summary.number = call.number;
            summary.name = resolve_syscall_name(model, call.number);
        }

        summary.call_count += 1;
        if (call.is_error) {
            summary.error_count += 1;
        }

        summary.wall_ns = saturating_add_u64(summary.wall_ns, clipped_wall);
        summary.sleep_ns = saturating_add_u64(summary.sleep_ns, clipped_sleep);
        summary.max_duration_ns = std::max(summary.max_duration_ns, clipped_wall);
    }

    view.syscall_summaries.reserve(syscalls_by_number.size());
    for (auto& [num, summary] : syscalls_by_number) {
        (void)num;
        view.syscall_summaries.push_back(std::move(summary));
    }

    std::sort(view.syscall_summaries.begin(), view.syscall_summaries.end(),
              [](const ktrace_syscall_summary& lhs, const ktrace_syscall_summary& rhs) {
                  if (lhs.wall_ns != rhs.wall_ns) {
                      return lhs.wall_ns > rhs.wall_ns;
                  }

                  return lhs.call_count != rhs.call_count ? (lhs.call_count > rhs.call_count)
                                                          : (lhs.number < rhs.number);
              });

    std::vector<flame_tree_node> tree_nodes(1);
    populate_tree_from_kernel_activity(model, filter.start_ns, filter.end_ns,
                                       filter, thread_by_tid, recorder_pids, tree_nodes);

    uint64_t root_total_ns = 0;
    for (size_t child_idx : tree_nodes[0].children) {
        root_total_ns = saturating_add_u64(root_total_ns, tree_nodes[child_idx].total_ns);
    }

    if (root_total_ns > 0) {
        double root_total_double = static_cast<double>(root_total_ns);
        std::set<std::string> ancestors;
        std::map<std::string, ktrace_hotspot> hotspots_by_name;
        collect_hotspots_from_tree(tree_nodes, 0, ancestors, hotspots_by_name);

        view.function_hotspots.reserve(hotspots_by_name.size());
        for (auto& [func_name, hotspot] : hotspots_by_name) {
            (void)func_name;
            hotspot.self_percent = PERCENT_HUNDRED * static_cast<double>(hotspot.self_ns) / root_total_double;
            view.function_hotspots.push_back(std::move(hotspot));
        }

        std::sort(view.function_hotspots.begin(), view.function_hotspots.end(),
                  [](const ktrace_hotspot& lhs, const ktrace_hotspot& rhs) {
                      if (lhs.self_ns != rhs.self_ns) {
                          return lhs.self_ns > rhs.self_ns;
                      }

                      return lhs.total_ns != rhs.total_ns ? (lhs.total_ns > rhs.total_ns) : (lhs.name < rhs.name);
                  });
    }

    return view;
}
} // namespace sheen
