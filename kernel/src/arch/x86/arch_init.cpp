#include <types.h>
#include <serial/serial.h>
#include <arch/percpu.h>
#include <arch/x86/gdt/gdt.h>
#include <arch/x86/idt/idt.h>
#include <arch/x86/fsgsbase.h>

uint8_t g_default_bsp_kernel_stack[0x2000];
namespace arch {
__PRIVILEGED_CODE
void arch_init() {
    // Setup kernel stack
    uint64_t kernel_bsp_stack_top = reinterpret_cast<uint64_t>(g_default_bsp_kernel_stack) +
                                    sizeof(g_default_bsp_kernel_stack);

    // Setup the GDT with userspace support
    x86::init_gdt(0, kernel_bsp_stack_top);
    
    // Setup the IDT and enable interrupts
    x86::init_idt();
    enable_interrupts();

    // Setup per-cpu area for the bootstrapping processor
    x86::enable_fsgsbase();
    init_bsp_per_cpu_area();
}
} // namespace arch
