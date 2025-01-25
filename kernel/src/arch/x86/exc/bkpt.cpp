#ifdef ARCH_X86_64
#include <arch/x86/exc/bkpt.h>
#include <process/ptregs.h>
#include <serial/serial.h>
#include <memory/memory.h>
#include <gdb/gdb_stub.h>

namespace arch::x86 {
DEFINE_INT_HANDLER(exc_breakpoint_handler) {
    __unused cookie;

#if 0
    serial::printf("Breakpoint exception: Entering GDB stub...\n");
#endif

    auto stub = gdb_stub::get();
    if (!stub) {
        stub = gdb_stub::create(serial::g_kernel_gdb_stub_uart_port);
    }

    stub->run_handler(regs);
    return IRQ_HANDLED;
}

// Debug Exception Handler (Single-Step)
DEFINE_INT_HANDLER(exc_debug_handler) {
    __unused cookie;

    // Clear the trap flag from EFLAGS register
    regs->hwframe.rflags &= ~((uint64_t)0x100);

#if 0
    serial::printf("Debug exception (single-step): Entering GDB stub...\n");
#endif

    auto stub = gdb_stub::get();
    if (!stub) {
        return IRQ_HANDLED;
    }

    stub->run_handler(regs);
    return IRQ_HANDLED;
}
} // namespace arch::x86

#endif // ARCH_X86_64
