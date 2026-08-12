#ifndef STELLUX_ARCH_AARCH64_SIGNAL_SIGFRAME_H
#define STELLUX_ARCH_AARCH64_SIGNAL_SIGFRAME_H

#include "common/types.h"
#include "signals/signal_types.h"

namespace aarch64 {

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

constexpr uint32_t FPSIMD_MAGIC = 0x46508001;

// FP/SIMD block for the mcontext reserved area. Field order is the kernel
// ABI (fpsr/fpcr before vregs), unlike sched::fpu_state, so delivery converts.
struct fpsimd_context {
    uint32_t magic;
    uint32_t size;
    uint32_t fpsr;
    uint32_t fpcr;
    uint8_t  vregs[32][16]; // V0-V31, 128-bit each
};

static_assert(__builtin_offsetof(fpsimd_context, fpsr) == 0x08);
static_assert(__builtin_offsetof(fpsimd_context, fpcr) == 0x0C);
static_assert(__builtin_offsetof(fpsimd_context, vregs) == 0x10);
static_assert(sizeof(fpsimd_context) == 528);

// Interrupted registers, matching the kernel sigcontext / musl mcontext_t.
// __reserved holds the fpsimd_context, 16-byte aligned per the ABI.
struct alignas(16) sigcontext {
    uint64_t fault_address;
    uint64_t regs[31]; // x0-x30
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t __pad0; // aligns __reserved to 16
    uint8_t  __reserved[4096];
};

static_assert(__builtin_offsetof(sigcontext, regs) == 0x08);
static_assert(__builtin_offsetof(sigcontext, sp) == 0x100);
static_assert(__builtin_offsetof(sigcontext, pc) == 0x108);
static_assert(__builtin_offsetof(sigcontext, pstate) == 0x110);
static_assert(__builtin_offsetof(sigcontext, __reserved) == 0x120);
static_assert(sizeof(sigcontext) == 4384);

// uc_sigmask is 8 bytes plus reserved space so uc_mcontext lands at the
// offset the 16-byte-aligned sigcontext requires.
struct ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp;    // uc_stack.ss_sp
    uint32_t ss_flags; // uc_stack.ss_flags
    uint32_t __pad0;
    uint64_t ss_size;  // uc_stack.ss_size
    signals::sig_set_t uc_sigmask; // blocked mask saved across the handler
    uint8_t  __sigmask_reserved[128];
    sigcontext uc_mcontext;
};

static_assert(__builtin_offsetof(ucontext, uc_sigmask) == 0x28);
static_assert(__builtin_offsetof(ucontext, uc_mcontext) == 0xB0);
static_assert(sizeof(ucontext) == 4560);

// Written to the user stack at delivery: siginfo first, then the context.
struct rt_sigframe {
    siginfo  info;
    ucontext uc;
};

static_assert(__builtin_offsetof(rt_sigframe, uc) == 0x80);
static_assert(__builtin_offsetof(rt_sigframe, uc) +
              __builtin_offsetof(ucontext, uc_mcontext) == 304);
static_assert(sizeof(rt_sigframe) == 4688);

} // namespace aarch64

#endif // STELLUX_ARCH_AARCH64_SIGNAL_SIGFRAME_H
