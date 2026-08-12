#ifndef STELLUX_ARCH_X86_64_SIGNAL_SIGFRAME_H
#define STELLUX_ARCH_X86_64_SIGNAL_SIGFRAME_H

#include "common/types.h"
#include "signals/signal_types.h"

namespace x86 {

constexpr int32_t SI_USER = 0;

// Delivered to SA_SIGINFO handlers. Only si_signo and si_code are filled,
// sender identity stays zero because standard signals carry no queue.
struct siginfo {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  __pad0;
    int32_t  si_pid;
    uint32_t si_uid;
    uint8_t  __pad[128 - 24];
};

static_assert(__builtin_offsetof(siginfo, si_signo) == 0x00);
static_assert(__builtin_offsetof(siginfo, si_code) == 0x08);
static_assert(__builtin_offsetof(siginfo, si_pid) == 0x10);
static_assert(__builtin_offsetof(siginfo, si_uid) == 0x14);
static_assert(sizeof(siginfo) == 128);

// Saved registers. Field order overlays musl's mcontext_t gregs[] and the
// kernel sigcontext so a handler reads gregs[REG_x] as the matching value.
struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15; // gregs[0..7]
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp; // gregs[8..15]
    uint64_t rip;    // gregs[16]
    uint64_t eflags; // gregs[17]
    uint16_t cs, gs, fs, ss; // gregs[18], packed as csgsfs
    uint64_t err, trapno, oldmask, cr2; // gregs[19..22]
    uint64_t fpstate; // address of a 512-byte FXSAVE image, 0 if none
    uint64_t __reserved1[8];
};

static_assert(__builtin_offsetof(sigcontext, rsp) == 15 * 8);    // REG_RSP
static_assert(__builtin_offsetof(sigcontext, rip) == 16 * 8);    // REG_RIP
static_assert(__builtin_offsetof(sigcontext, eflags) == 17 * 8); // REG_EFL
static_assert(__builtin_offsetof(sigcontext, cs) == 18 * 8);     // REG_CSGSFS
static_assert(__builtin_offsetof(sigcontext, cr2) == 22 * 8);    // REG_CR2
static_assert(sizeof(sigcontext) == 256);

struct ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp;    // uc_stack.ss_sp
    uint32_t ss_flags; // uc_stack.ss_flags
    uint32_t __pad0;
    uint64_t ss_size;  // uc_stack.ss_size
    sigcontext uc_mcontext;
    signals::sig_set_t uc_sigmask; // blocked mask saved across the handler
};

static_assert(__builtin_offsetof(ucontext, uc_mcontext) == 0x28);
static_assert(__builtin_offsetof(ucontext, uc_sigmask) == 0x128);
static_assert(sizeof(ucontext) == 304);

// Written to the user stack at delivery. The FXSAVE image sits separately
// above the frame, which needs stricter alignment than the frame base.
struct rt_sigframe {
    uint64_t pretcode; // restorer address, the handler's return address
    ucontext uc;
    siginfo  info;
};

static_assert(__builtin_offsetof(rt_sigframe, uc) == 0x08);
static_assert(__builtin_offsetof(rt_sigframe, info) == 0x138);
static_assert(sizeof(rt_sigframe) == 440);

} // namespace x86

#endif // STELLUX_ARCH_X86_64_SIGNAL_SIGFRAME_H
