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

uint8_t g_default_bsp_system_stack[PAGE_SIZE];

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

    // Setup BSP's idle task (current) and system stack reference
    task_control_block* bsp_idle_task = sched::get_idle_task(BSP_CPU_ID);
    zeromem(bsp_idle_task, sizeof(task_control_block));
    this_cpu_write(current_task, bsp_idle_task);
    this_cpu_write(current_system_stack, bsp_system_stack_top);

    current->system_stack_top = bsp_system_stack_top;
    current->cpu = 0;
    current->elevated = 1;
    current->state = process_state::RUNNING;
    current->pid = 0;

    // Enable the syscall interface
    enable_syscall_interface();

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
    entry.destination = current->cpu;       // Route to the current CPU

    // Create an IOAPIC redirection entry
    ioapic->write_redirection_entry(com1_gsi, &entry);

    // Register the interrupt handler in the IDT
    register_irq_handler(entry.vector, input::__com1_irq_handler, true, nullptr);
}
} // namespace arch
