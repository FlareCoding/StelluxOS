#ifndef STELLUX_SIGNALS_SIGNAL_TYPES_H
#define STELLUX_SIGNALS_SIGNAL_TYPES_H

#include "common/types.h"
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

// Bitmask of signals 1..64: bit (N-1) represents signal N
using sig_set_t = uint64_t;

constexpr sig_set_t sig_bit(uint32_t sig) {
    return 1ULL << (sig - 1);
}

constexpr bool sig_valid(uint32_t sig) {
    return sig >= 1 && sig <= NSIG;
}

// POSIX: SIGKILL and SIGSTOP can never be blocked, caught, or ignored
constexpr sig_set_t UNBLOCKABLE_MASK = sig_bit(SIGKILL) | sig_bit(SIGSTOP);

// What SIG_DFL means for each signal. Stop-class defaults are not
// implemented, so callers pick their own fallback where one is needed.
enum class default_action : uint8_t {
    TERM,
    IGNORE,
    STOP,
};

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
// blocked is written only by the owning task.
struct task_signals {
    sig_set_t blocked;
    sig_set_t pending;
};

// Per-process signal state shared by all threads in a thread group.
struct group_signals {
    sync::spinlock lock; // guards actions
    sig_set_t shared_pending;
    k_sigaction actions[NSIG];
};

} // namespace signals

#endif // STELLUX_SIGNALS_SIGNAL_TYPES_H
