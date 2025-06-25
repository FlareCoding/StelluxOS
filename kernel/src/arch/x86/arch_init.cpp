#include <types.h>
#include <memory/memory.h>
#include <memory/paging.h>
#include <serial/serial.h>
#include <arch/percpu.h>
#include <arch/x86/gdt/gdt.h>
#include <arch/x86/idt/idt.h>
#include <arch/x86/cpuid.h>
#include <arch/x86/fsgsbase.h>
#include <arch/x86/pat.h>
#include <arch/x86/apic/lapic.h>
#include <arch/x86/apic/ioapic.h>
#include <syscall/syscalls.h>
#include <sched/sched.h>
#include <dynpriv/dynpriv.h>
#include <input/serial_irq.h>
#include <process/process.h>
#include <process/process_env.h>

uint8_t g_default_bsp_system_stack[PAGE_SIZE * 4];

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

    // Enable fsgsbase instructions if they are supported
    if (x86::cpuid_is_fsgsbase_supported()) {
        x86::enable_fsgsbase();
    }

    // Setup per-cpu area for the bootstrapping processor
    init_bsp_per_cpu_area();

    // Initialize the idle process environment
    new (&g_idle_process_env) process_env(
        process_creation_flags::IS_KERNEL |
        process_creation_flags::IS_IDLE
    );

    // Initialize the BSP's idle process core
    process_core* bsp_idle_core = sched::get_idle_process_core(BSP_CPU_ID);
    zeromem(bsp_idle_core, sizeof(process_core));
    bsp_idle_core->state = process_state::RUNNING;
    bsp_idle_core->hw_state.cpu = BSP_CPU_ID;
    bsp_idle_core->hw_state.elevated = 1;
    bsp_idle_core->stacks.task_stack_top = bsp_system_stack_top;
    bsp_idle_core->stacks.system_stack_top = bsp_system_stack_top;

    // Create the BSP's idle process
    process* bsp_idle_task = sched::get_idle_process(BSP_CPU_ID);
    bsp_idle_task->init(bsp_idle_core, false, &g_idle_process_env, false);

    // Set up the current process and system stack
    this_cpu_write(current_process, bsp_idle_task);
    this_cpu_write(current_process_core, bsp_idle_core);
    this_cpu_write(current_system_stack, bsp_system_stack_top);

    // Initialize FPU per-CPU tracking variables
    this_cpu_write(fpu_owner, bsp_idle_core);
    this_cpu_write(fpu_used_in_irq, false);

    // Enable the syscall interface
    enable_syscall_interface();

    // Initialize the syscall table
    init_syscall_table();

    // Setup and enable dynamic privilege mechanism
    dynpriv::set_blessed_kernel_asid();
}

__PRIVILEGED_CODE
void arch_late_stage_init() {
    auto& lapic = x86::lapic::get();
    if (!lapic.get()) {
        serial::printf("[!] Failed to initialize local APIC\n");
        return;
    }

    lapic->init();
}

__PRIVILEGED_CODE
void setup_com1_irq() {
    // Get IOAPIC instance
    auto ioapic = arch::x86::ioapic::get();
    if (!ioapic) {
        return;
    }

    // Calculate the Global System Interrupt (GSI) for COM1 (IRQ4)
    uint8_t com1_irq = 4;
    uint32_t com1_gsi = ioapic->get_global_interrupt_base() + com1_irq;

    // Create a redirection entry for COM1
    arch::x86::ioapic::redirection_entry entry;
    zeromem(&entry, sizeof(arch::x86::ioapic::redirection_entry));

    entry.vector = find_free_irq_vector();  // Assign an IDT vector
    entry.delv_mode = 0b000;                // Fixed delivery mode
    entry.dest_mode = 0;                    // Physical mode
    entry.trigger_mode = 0;                 // Edge-triggered
    entry.mask = 0;                         // Enable the interrupt
    entry.destination = current->get_core()->hw_state.cpu;  // Route to the current CPU

    // Create an IOAPIC redirection entry
    ioapic->write_redirection_entry(com1_gsi, &entry);

    // Register the interrupt handler in the IDT
    register_irq_handler(entry.vector, input::__com1_irq_handler, true, nullptr);
}
} // namespace arch
