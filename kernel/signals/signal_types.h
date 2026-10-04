#ifndef STELLUX_SIGNALS_SIGNAL_TYPES_H
#define STELLUX_SIGNALS_SIGNAL_TYPES_H

#include "common/types.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"

namespace signals {

// Signal numbers matching the musl ABI
constexpr uint32_t SIGHUP   = 1;
constexpr uint32_t SIGINT   = 2;
constexpr uint32_t SIGQUIT  = 3;
constexpr uint32_t SIGILL   = 4;
constexpr uint32_t SIGTRAP  = 5;
constexpr uint32_t SIGABRT  = 6;
constexpr uint32_t SIGBUS   = 7;
constexpr uint32_t SIGFPE   = 8;
constexpr uint32_t SIGKILL  = 9;
constexpr uint32_t SIGUSR1  = 10;
constexpr uint32_t SIGSEGV  = 11;
constexpr uint32_t SIGUSR2  = 12;
constexpr uint32_t SIGPIPE  = 13;
constexpr uint32_t SIGALRM  = 14;
constexpr uint32_t SIGTERM  = 15;
constexpr uint32_t SIGCHLD  = 17;
constexpr uint32_t SIGCONT  = 18;
constexpr uint32_t SIGSTOP  = 19;
constexpr uint32_t SIGTSTP  = 20;
constexpr uint32_t SIGTTIN  = 21;
constexpr uint32_t SIGTTOU  = 22;
constexpr uint32_t SIGURG   = 23;
constexpr uint32_t SIGWINCH = 28;
constexpr uint32_t NSIG     = 64; // highest valid signal number

// User handler sentinels (match musl SIG_DFL / SIG_IGN)
constexpr uintptr_t SIG_DFL = 0;
constexpr uintptr_t SIG_IGN = 1;

// sigaction flags (musl ABI subset the kernel interprets)
constexpr uint64_t SA_SIGINFO   = 0x00000004;
constexpr uint64_t SA_RESTORER  = 0x04000000;
constexpr uint64_t SA_ONSTACK   = 0x08000000;
constexpr uint64_t SA_RESTART   = 0x10000000;
constexpr uint64_t SA_NODEFER   = 0x40000000;
constexpr uint64_t SA_RESETHAND = 0x80000000;

// si_code values reported to handlers (musl ABI), each scoped to its signal
constexpr int32_t SI_USER     = 0;
constexpr int32_t SI_KERNEL   = 0x80;
constexpr int32_t ILL_ILLOPC  = 1;
constexpr int32_t FPE_INTDIV  = 1;
constexpr int32_t FPE_INTOVF  = 2;
constexpr int32_t FPE_FLTDIV  = 3;
constexpr int32_t FPE_FLTOVF  = 4;
constexpr int32_t FPE_FLTUND  = 5;
constexpr int32_t FPE_FLTRES  = 6;
constexpr int32_t FPE_FLTINV  = 7;
constexpr int32_t SEGV_MAPERR = 1;
constexpr int32_t SEGV_ACCERR = 2;
constexpr int32_t BUS_ADRALN  = 1;
constexpr int32_t BUS_ADRERR  = 2;
constexpr int32_t BUS_OBJERR  = 3;
constexpr int32_t TRAP_BRKPT  = 1;
constexpr int32_t TRAP_TRACE  = 2;

// Bitmask of signals 1..64: bit (N-1) represents signal N
using sig_set_t = uint64_t;

// What SIG_DFL means for each signal. Stop-class defaults are not
// implemented, so callers pick their own fallback where one is needed.
enum class default_action : uint8_t {
    TERM,
    IGNORE,
    STOP,
};

// Kernel-side layout matches the musl k_sigaction struct
// passed to rt_sigaction (handler, flags, restorer, 64-bit mask).
struct k_sigaction {
    uintptr_t handler;
    uint64_t  flags;
    uintptr_t restorer;
    sig_set_t mask;
};

static_assert(sizeof(k_sigaction) == 32, "k_sigaction must match musl rt_sigaction layout");

// Per-task signal state. pending is set by senders,
// the masks are written only by the owning task.
struct task_signals {
    sync::atomic<sig_set_t> blocked;
    sync::atomic<sig_set_t> pending;
    sig_set_t               saved_mask = 0;
    bool                    restore_mask = false;
};

// Per-process signal state shared by all threads in a thread group.
struct group_signals {
    sync::spinlock lock; // guards actions
    sync::atomic<sig_set_t> shared_pending;
    sync::atomic<uint32_t> exit_signal; // first fatal signal that began group termination, 0 if none
    k_sigaction actions[NSIG];
};

// A synchronous fault raised by user code, as the signal it raises
struct fault_signal {
    uint32_t  sig;
    int32_t   code; // si_code
    uintptr_t addr; // si_addr, the faulting instruction or data address
};

constexpr sig_set_t sig_bit(uint32_t sig) {
    return 1ULL << (sig - 1);
}

constexpr bool sig_valid(uint32_t sig) {
    return sig >= 1 && sig <= NSIG;
}

// POSIX: SIGKILL and SIGSTOP can never be blocked, caught, or ignored
constexpr sig_set_t UNBLOCKABLE_MASK = sig_bit(SIGKILL) | sig_bit(SIGSTOP);

constexpr default_action dfl_action(uint32_t sig) {
    switch (sig) {
        case SIGCHLD:
        case SIGCONT:
        case SIGURG:
        case SIGWINCH:
            return default_action::IGNORE;
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            return default_action::STOP;
        default:
            return default_action::TERM;
    }
}

} // namespace signals

#endif // STELLUX_SIGNALS_SIGNAL_TYPES_H
