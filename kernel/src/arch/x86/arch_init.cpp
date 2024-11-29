#include <types.h>
#include <serial/serial.h>
#include <arch/x86/gdt/gdt.h>

namespace arch {
void arch_init() {
    x86::init_gdt(0, 0);
}
}
