#ifdef ARCH_X86_64
#include <syscall/syscalls.h>
#include <arch/x86/gdt/gdt.h>
#include <arch/x86/msr.h>

EXTERN_C void asm_syscall_entry();

// Architecture-specific code for enabling the syscall interface
namespace arch {
__PRIVILEGED_CODE
void enable_syscall_interface() {
    uint64_t star_reg_val = (((uint64_t)__TSS_PT2_SELECTOR | 3) << 48) | ((uint64_t)__KERNEL_CS << 32);

    // Setup syscall related MSRs
    x86::msr::write(IA32_STAR, star_reg_val);
    x86::msr::write(IA32_LSTAR, (uintptr_t)asm_syscall_entry);
    x86::msr::write(IA32_FMASK, 0x200);

    // Enable syscall instruction
    uint64_t efer = x86::msr::read(IA32_EFER);
    efer |= IA32_EFER_SCE;
    x86::msr::write(IA32_EFER, efer);
}
} // namespace arch

#endif // ARCH_X86_64

