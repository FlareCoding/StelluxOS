#include "syscall/syscall.h"
#include "defs/segments.h"
#include "hw/msr.h"
#include "common/utils/logging.h"

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
     * Our GDT layout (selectors with RPL=0 or RPL=3):
     *   0x08 = KERNEL_CS
     *   0x10 = KERNEL_DS
     *   0x1B = USER_DS (GDT index 3, RPL=3)
     *   0x23 = USER_CS (GDT index 4, RPL=3)
     *
     * For SYSCALL (to kernel): We want CS=0x08 (KERNEL_CS), SS=0x10 (KERNEL_DS)
     *   -> STAR[47:32] = 0x08
     *
     * For SYSRET (64-bit to user): We want CS=0x23 (USER_CS), SS=0x1B (USER_DS)
     *   Hardware applies RPL=3 automatically. Base value should be such that:
     *   base + 16 = 0x20 (USER_CS selector without RPL)
     *   base + 8  = 0x18 (USER_DS selector without RPL)
     *   -> base = 0x10 (GDT_KERNEL_DS_IDX * 8)
     *   -> STAR[63:48] = 0x10
     */
    uint64_t star = (static_cast<uint64_t>(x86::KERNEL_DS) << 48) | // SYSRET base
                    (static_cast<uint64_t>(x86::KERNEL_CS) << 32);  // SYSCALL base
    msr::write(MSR_STAR, star);

    /* LSTAR: 64-bit SYSCALL entry point */
    msr::write(MSR_LSTAR, reinterpret_cast<uint64_t>(stlx_x86_syscall_entry));

    /* CSTAR: 32-bit compat mode entry (not used, but set to something safe) */
    msr::write(MSR_CSTAR, 0);

    /* SFMASK: RFLAGS bits to clear on SYSCALL (IF, DF, TF) */
    msr::write(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF);

    log::info("syscall: x86_64 initialized (LSTAR=0x%lx)",
              reinterpret_cast<uint64_t>(stlx_x86_syscall_entry));

    return OK;
}

} // namespace syscall
