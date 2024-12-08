#include <types.h>
#include <memory/memory.h>
#include <serial/serial.h>
#include <arch/percpu.h>
#include <arch/x86/gdt/gdt.h>
#include <arch/x86/idt/idt.h>
#include <arch/x86/fsgsbase.h>
#include <arch/x86/pat.h>
#include <syscall/syscalls.h>
#include <sched/sched.h>
#include <dynpriv/dynpriv.h>

uint8_t g_default_bsp_system_stack[0x2000];

namespace arch {
__PRIVILEGED_CODE
void arch_init() {
    // Setup kernel stack
    uint64_t bsp_system_stack_top = reinterpret_cast<uint64_t>(g_default_bsp_system_stack) +
                                    sizeof(g_default_bsp_system_stack) - 0x10;

    // Setup the GDT with userspace support
    x86::init_gdt(BSP_CPU_ID, bsp_system_stack_top);
    
    // Setup the IDT and enable interrupts
    x86::init_idt();
    enable_interrupts();

    // Setup the kernel PAT to contain a write-combining entry
    x86::setup_kernel_pat();

    // Setup per-cpu area for the bootstrapping processor
    x86::enable_fsgsbase();
    init_bsp_per_cpu_area();

    // Setup BSP's idle task (current)
    task_control_block* bsp_idle_task = sched::get_idle_task(BSP_CPU_ID);
    zeromem(bsp_idle_task, sizeof(task_control_block));
    this_cpu_write(current_task, bsp_idle_task);

    current->system_stack = bsp_system_stack_top;
    current->cpu = 0;
    current->elevated = 1;
    current->state = process_state::RUNNING;
    current->pid = 0;

    // Enable the syscall interface
    enable_syscall_interface();

    // Setup and enable dynamic privilege mechanism
    dynpriv::use_current_asid();
}
} // namespace arch
