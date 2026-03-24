#include "syscall/syscall.h"
#include "defs/segments.h"
#include "hw/msr.h"

extern "C" void stlx_x86_syscall_entry();

namespace {

constexpr uint32_t MSR_EFER   = 0xC0000080;
constexpr uint32_t MSR_STAR   = 0xC0000081;
constexpr uint32_t MSR_LSTAR  = 0xC0000082;
constexpr uint32_t MSR_CSTAR  = 0xC0000083;
constexpr uint32_t MSR_SFMASK = 0xC0000084;

constexpr uint64_t EFER_SCE = (1 << 0);  // System Call Enable

constexpr uint64_t RFLAGS_IF = (1 << 9);
constexpr uint64_t RFLAGS_DF = (1 << 10);
constexpr uint64_t RFLAGS_TF = (1 << 8);

// SYSRET derives selectors from IA32_STAR[63:48]:
//   CS = STAR[63:48] + 16 (then CPL/RPL = 3)
//   SS = STAR[63:48] + 8
//
// Linux programs this field using a user-space selector base, not a kernel one.
// On this machine the old KERNEL_DS-based programming empirically returned with
// SS=0x18 instead of USER_DS=0x1B, leading to #GP on the next IRETQ. Using the
// user selector base makes both derived selectors land on the intended user
// segments:
//   base + 8  == USER_DS
//   base + 16 == USER_CS
constexpr uint16_t SYSRET_SEL_BASE = static_cast<uint16_t>(x86::USER_DS - 8);
static_assert(static_cast<uint16_t>(SYSRET_SEL_BASE + 8) == x86::USER_DS,
              "SYSRET base must derive USER_DS");
static_assert(static_cast<uint16_t>(SYSRET_SEL_BASE + 16) == x86::USER_CS,
              "SYSRET base must derive USER_CS");

} // anonymous namespace

namespace syscall {

__PRIVILEGED_CODE int32_t init_arch_syscalls() {
    /* Enable SYSCALL/SYSRET in EFER */
    uint64_t efer = msr::read(MSR_EFER);
    msr::write(MSR_EFER, efer | EFER_SCE);

    /*
     * STAR MSR layout:
     *   bits 31:0  - reserved (32-bit SYSCALL target EIP, not used in long mode)
     *   bits 47:32 - SYSCALL CS/SS base (CS = value, SS = value + 8)
     *   bits 63:48 - SYSRET CS/SS base (64-bit: CS = value + 16, SS = value + 8)
     *
     * Our GDT layout:
     *   0x08 = KERNEL_CS
     *   0x10 = KERNEL_DS
     *   0x1B = USER_DS
     *   0x23 = USER_CS
     *
     * For SYSCALL (to kernel): STAR[47:32] = KERNEL_CS.
     * For SYSRET (to user): use a base that derives USER_DS/USER_CS directly.
     */
    uint64_t star = (static_cast<uint64_t>(SYSRET_SEL_BASE) << 48) | // SYSRET base
                    (static_cast<uint64_t>(x86::KERNEL_CS) << 32);  // SYSCALL base
    msr::write(MSR_STAR, star);

    /* LSTAR: 64-bit SYSCALL entry point */
    msr::write(MSR_LSTAR, reinterpret_cast<uint64_t>(stlx_x86_syscall_entry));

    /* CSTAR: 32-bit compat mode entry (not used, but set to something safe) */
    msr::write(MSR_CSTAR, 0);

    /* SFMASK: RFLAGS bits to clear on SYSCALL (IF, DF, TF) */
    msr::write(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF);

    return OK;
}

} // namespace syscall
