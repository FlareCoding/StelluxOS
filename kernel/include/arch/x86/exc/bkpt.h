#ifdef ARCH_X86_64
#ifndef BKPT_H
#define BKPT_H
#include <interrupts/irq.h>

namespace arch::x86 {
DEFINE_INT_HANDLER(exc_breakpoint_handler);
DEFINE_INT_HANDLER(exc_debug_handler);
} // namespace arch::x86

#endif // BKPT_H
#endif // ARCH_X86_64