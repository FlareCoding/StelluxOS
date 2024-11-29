#include <types.h>
#include <serial/serial.h>
#include <arch/x86/gdt/gdt.h>

uint8_t g_default_bsp_kernel_stack[0x2000];
namespace arch {
void arch_init() {
    // Setup kernel stack
    uint64_t kernel_bsp_stack_top = reinterpret_cast<uint64_t>(g_default_bsp_kernel_stack) +
                                    sizeof(g_default_bsp_kernel_stack);

    x86::init_gdt(0, kernel_bsp_stack_top);
}
}
