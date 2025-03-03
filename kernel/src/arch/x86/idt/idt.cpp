#ifdef ARCH_X86_64
#include <arch/x86/idt/idt.h>
#include <arch/x86/apic/lapic.h>
#include <arch/x86/exc/bkpt.h>
#include <memory/memory.h>
#include <core/klog.h>
#include <sched/sched.h>

#define MAX_IRQS 64

#define PANIC_IRQ IRQ17

#define RESERVED_IRQ_VECTOR 0xff

EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_div();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_db();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_nmi();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_bp();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_of();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_br();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_ud();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_nm();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_df();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_cso();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_ts();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_np();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_ss();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_gp();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_pf();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_mf();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_ac();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_mc();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_xm();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_ve();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_cp();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_hv();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_vc();
EXTERN_C __PRIVILEGED_CODE void asm_exc_handler_sx();
 
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_0();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_1();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_2();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_3();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_4();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_5();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_6();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_7();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_8();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_9();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_10();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_11();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_12();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_13();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_14();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_15();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_16();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_17();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_18();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_19();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_20();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_21();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_22();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_23();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_24();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_25();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_26();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_27();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_28();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_29();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_30();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_31();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_32();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_33();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_34();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_35();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_36();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_37();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_38();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_39();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_40();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_41();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_42();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_43();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_44();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_45();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_46();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_47();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_48();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_49();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_50();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_51();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_52();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_53();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_54();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_55();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_56();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_57();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_58();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_59();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_60();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_61();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_62();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_63();
EXTERN_C __PRIVILEGED_CODE void asm_irq_handler_64();

namespace arch::x86 {
const char* g_cpu_exception_strings[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",

    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",

    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",

    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Manual Panic"
};

DECLARE_GLOBAL_OBJECT(mutex, g_panic_lock);

struct int_exc_handlers {
    irq_handler_t divide_by_zero;
    irq_handler_t debug;
    irq_handler_t nmi;
    irq_handler_t breakpoint;
    irq_handler_t overflow;
    irq_handler_t bound_range;
    irq_handler_t invalid_opcode;
    irq_handler_t device_not_available;
    irq_handler_t double_fault;
    irq_handler_t coprocessor_seg_overrun;
    irq_handler_t invalid_tss;
    irq_handler_t segment_not_present;
    irq_handler_t stack_fault;
    irq_handler_t general_protection_fault;
    irq_handler_t page_fault;
};
static_assert(sizeof(int_exc_handlers) == 15 * sizeof(irq_handler_t));

struct irq_handler_descriptor_table {
    irq_desc descriptors[MAX_IRQS];
};
static_assert(sizeof(irq_handler_descriptor_table) == MAX_IRQS * sizeof(irq_desc));

__PRIVILEGED_DATA
static int_exc_handlers g_int_exc_handlers = {
    .divide_by_zero = 0,
    .debug = exc_debug_handler,
    .nmi = 0,
    .breakpoint = exc_breakpoint_handler,
    .overflow = 0,
    .bound_range = 0,
    .invalid_opcode = 0,
    .device_not_available = 0,
    .double_fault = 0,
    .coprocessor_seg_overrun = 0,
    .invalid_tss = 0,
    .segment_not_present = 0,
    .stack_fault = 0,
    .general_protection_fault = 0,
    .page_fault = 0
};

__PRIVILEGED_DATA
static irq_handler_descriptor_table g_irq_handler_table;

__PRIVILEGED_DATA
static idt_desc g_kernel_idt_descriptor = {
    .limit = 0,
    .base = 0
};
__PRIVILEGED_DATA
static interrupt_descriptor_table g_kernel_idt;

// Common entry point for cpu exceptions
__PRIVILEGED_CODE
void common_exc_entry(ptregs* regs) {
    irq_handler_t* handlers = (irq_handler_t*)&g_int_exc_handlers;
    if (handlers[regs->intno] != NULL) {
        irqreturn_t ret = handlers[regs->intno](regs, nullptr);
        __unused ret;
        return;
    }

    panic(regs);
}

// Common entry point for IRQs
__PRIVILEGED_CODE
void common_irq_entry(ptregs* regs) {
    // Check if a panic was manually induced
    if (regs->intno == PANIC_IRQ) {
        // Patch the irq number to get the appropriate
        // string when debug printing later.
        regs->intno = 31;

        // Call the panic
        panic(regs);
    }

    uint64_t irq_index = regs->intno - IRQ0;

    irq_desc* desc = &g_irq_handler_table.descriptors[irq_index];
    if (!desc->handler) {
        return;
    }

    if (desc->fast_apic_eoi) {
        lapic::get()->complete_irq();
    }

    irqreturn_t ret = desc->handler(regs, desc->cookie);
    __unused ret;
}

// Common entry point for all interrupt service routines
EXTERN_C
__PRIVILEGED_CODE
void common_isr_entry(ptregs regs) {
    // After faking out being elevated, original elevation
    // privileged should be restored, except in the case
    // when the scheduler switched context into a new task.
    int original_elevate_status = current->elevated;
    task_control_block* original_task = current;

    // Fake out being elevated if needed because the code has to
    // be able to tell if it's running in privileged mode or not.
    original_task->elevated = 1;

    // Check whether the interrupt is an IRQ or a trap/exception
    if (regs.intno >= IRQ0) {
        common_irq_entry(&regs);
    } else {
        common_exc_entry(&regs);
    }

    // Restore the original elevate status
    original_task->elevated = original_elevate_status;
}

__PRIVILEGED_CODE
void init_idt() {
    g_kernel_idt_descriptor.limit = sizeof(g_kernel_idt) - 1;
    g_kernel_idt_descriptor.base = reinterpret_cast<uint64_t>(&g_kernel_idt);

    // Perform initial setup of the IRQ handler table
    zeromem(&g_irq_handler_table, sizeof(irq_handler_descriptor_table));

    // Set exception handlers
    SET_KERNEL_INTERRUPT_GATE(EXC_DIVIDE_BY_ZERO,           asm_exc_handler_div);
    SET_USER_INTERRUPT_GATE(EXC_DEBUG,                      asm_exc_handler_db);
    SET_KERNEL_INTERRUPT_GATE(EXC_NMI,                      asm_exc_handler_nmi);
    SET_USER_INTERRUPT_GATE(EXC_BREAKPOINT,                 asm_exc_handler_bp);
    SET_KERNEL_INTERRUPT_GATE(EXC_OVERFLOW,                 asm_exc_handler_of);
    SET_KERNEL_INTERRUPT_GATE(EXC_BOUND_RANGE,              asm_exc_handler_br);
    SET_KERNEL_INTERRUPT_GATE(EXC_INVALID_OPCODE,           asm_exc_handler_ud);
    SET_KERNEL_INTERRUPT_GATE(EXC_DEVICE_NOT_AVAILABLE,     asm_exc_handler_nm);
    SET_KERNEL_INTERRUPT_GATE(EXC_DOUBLE_FAULT,             asm_exc_handler_df);
    SET_KERNEL_INTERRUPT_GATE(EXC_COPROCESSOR_SEG_OVERRUN,  asm_exc_handler_cso);
    SET_KERNEL_INTERRUPT_GATE(EXC_INVALID_TSS,              asm_exc_handler_ts);
    SET_KERNEL_INTERRUPT_GATE(EXC_SEGMENT_NOT_PRESENT,      asm_exc_handler_np);
    SET_KERNEL_INTERRUPT_GATE(EXC_STACK_FAULT,              asm_exc_handler_ss);
    SET_KERNEL_INTERRUPT_GATE(EXC_GENERAL_PROTECTION,       asm_exc_handler_gp);
    SET_KERNEL_INTERRUPT_GATE(EXC_PAGE_FAULT,               asm_exc_handler_pf);
    SET_KERNEL_INTERRUPT_GATE(EXC_X87_FLOATING_POINT,       asm_exc_handler_mf);
    SET_KERNEL_INTERRUPT_GATE(EXC_ALIGNMENT_CHECK,          asm_exc_handler_ac);
    SET_KERNEL_INTERRUPT_GATE(EXC_MACHINE_CHECK,            asm_exc_handler_mc);
    SET_KERNEL_INTERRUPT_GATE(EXC_SIMD_FLOATING_POINT,      asm_exc_handler_xm);
    SET_KERNEL_INTERRUPT_GATE(EXC_VIRTUALIZATION,           asm_exc_handler_ve);
    SET_KERNEL_INTERRUPT_GATE(EXC_SECURITY_EXCEPTION,       asm_exc_handler_cp);
    SET_KERNEL_INTERRUPT_GATE(EXC_HYPERVISOR_VIOLATION,     asm_exc_handler_hv);
    SET_KERNEL_INTERRUPT_GATE(EXC_VMM_COMMUNICATION,        asm_exc_handler_vc);
    SET_KERNEL_INTERRUPT_GATE(EXC_SECURITY_EXTENSION,       asm_exc_handler_sx);

    // Set IRQ Handlers
    SET_KERNEL_TRAP_GATE(IRQ0, asm_irq_handler_0);
    SET_KERNEL_TRAP_GATE(IRQ1, asm_irq_handler_1);
    SET_KERNEL_TRAP_GATE(IRQ2, asm_irq_handler_2);
    SET_KERNEL_TRAP_GATE(IRQ3, asm_irq_handler_3);
    SET_KERNEL_TRAP_GATE(IRQ4, asm_irq_handler_4);
    SET_KERNEL_TRAP_GATE(IRQ5, asm_irq_handler_5);
    SET_KERNEL_TRAP_GATE(IRQ6, asm_irq_handler_6);
    SET_KERNEL_TRAP_GATE(IRQ7, asm_irq_handler_7);
    SET_KERNEL_TRAP_GATE(IRQ8, asm_irq_handler_8);
    SET_KERNEL_TRAP_GATE(IRQ9, asm_irq_handler_9);
    SET_KERNEL_TRAP_GATE(IRQ10, asm_irq_handler_10);
    SET_KERNEL_TRAP_GATE(IRQ11, asm_irq_handler_11);
    SET_KERNEL_TRAP_GATE(IRQ12, asm_irq_handler_12);
    SET_KERNEL_TRAP_GATE(IRQ13, asm_irq_handler_13);
    SET_KERNEL_TRAP_GATE(IRQ14, asm_irq_handler_14);
    SET_KERNEL_TRAP_GATE(IRQ15, asm_irq_handler_15);

    // Scheduler IRQ that can be called from usermode
    SET_USER_INTERRUPT_GATE(IRQ16, asm_irq_handler_16);
    
    // Panic interrupt that can be called from usermode
    SET_USER_INTERRUPT_GATE(IRQ17, asm_irq_handler_17);

    SET_KERNEL_TRAP_GATE(IRQ18, asm_irq_handler_18);
    SET_KERNEL_TRAP_GATE(IRQ19, asm_irq_handler_19);
    SET_KERNEL_TRAP_GATE(IRQ20, asm_irq_handler_20);
    SET_KERNEL_TRAP_GATE(IRQ21, asm_irq_handler_21);
    SET_KERNEL_TRAP_GATE(IRQ22, asm_irq_handler_22);
    SET_KERNEL_TRAP_GATE(IRQ23, asm_irq_handler_23);
    SET_KERNEL_TRAP_GATE(IRQ24, asm_irq_handler_24);
    SET_KERNEL_TRAP_GATE(IRQ25, asm_irq_handler_25);
    SET_KERNEL_TRAP_GATE(IRQ26, asm_irq_handler_26);
    SET_KERNEL_TRAP_GATE(IRQ27, asm_irq_handler_27);
    SET_KERNEL_TRAP_GATE(IRQ28, asm_irq_handler_28);
    SET_KERNEL_TRAP_GATE(IRQ29, asm_irq_handler_29);
    SET_KERNEL_TRAP_GATE(IRQ30, asm_irq_handler_30);
    SET_KERNEL_TRAP_GATE(IRQ31, asm_irq_handler_31);
    SET_KERNEL_TRAP_GATE(IRQ32, asm_irq_handler_32);
    SET_KERNEL_TRAP_GATE(IRQ33, asm_irq_handler_33);
    SET_KERNEL_TRAP_GATE(IRQ34, asm_irq_handler_34);
    SET_KERNEL_TRAP_GATE(IRQ35, asm_irq_handler_35);
    SET_KERNEL_TRAP_GATE(IRQ36, asm_irq_handler_36);
    SET_KERNEL_TRAP_GATE(IRQ37, asm_irq_handler_37);
    SET_KERNEL_TRAP_GATE(IRQ38, asm_irq_handler_38);
    SET_KERNEL_TRAP_GATE(IRQ39, asm_irq_handler_39);
    SET_KERNEL_TRAP_GATE(IRQ40, asm_irq_handler_40);
    SET_KERNEL_TRAP_GATE(IRQ41, asm_irq_handler_41);
    SET_KERNEL_TRAP_GATE(IRQ42, asm_irq_handler_42);
    SET_KERNEL_TRAP_GATE(IRQ43, asm_irq_handler_43);
    SET_KERNEL_TRAP_GATE(IRQ44, asm_irq_handler_44);
    SET_KERNEL_TRAP_GATE(IRQ45, asm_irq_handler_45);
    SET_KERNEL_TRAP_GATE(IRQ46, asm_irq_handler_46);
    SET_KERNEL_TRAP_GATE(IRQ47, asm_irq_handler_47);
    SET_KERNEL_TRAP_GATE(IRQ48, asm_irq_handler_48);
    SET_KERNEL_TRAP_GATE(IRQ49, asm_irq_handler_49);
    SET_KERNEL_TRAP_GATE(IRQ50, asm_irq_handler_50);
    SET_KERNEL_TRAP_GATE(IRQ51, asm_irq_handler_51);
    SET_KERNEL_TRAP_GATE(IRQ52, asm_irq_handler_52);
    SET_KERNEL_TRAP_GATE(IRQ53, asm_irq_handler_53);
    SET_KERNEL_TRAP_GATE(IRQ54, asm_irq_handler_54);
    SET_KERNEL_TRAP_GATE(IRQ55, asm_irq_handler_55);
    SET_KERNEL_TRAP_GATE(IRQ56, asm_irq_handler_56);
    SET_KERNEL_TRAP_GATE(IRQ57, asm_irq_handler_57);
    SET_KERNEL_TRAP_GATE(IRQ58, asm_irq_handler_58);
    SET_KERNEL_TRAP_GATE(IRQ59, asm_irq_handler_59);
    SET_KERNEL_TRAP_GATE(IRQ60, asm_irq_handler_60);
    SET_KERNEL_TRAP_GATE(IRQ61, asm_irq_handler_61);
    SET_KERNEL_TRAP_GATE(IRQ62, asm_irq_handler_62);
    SET_KERNEL_TRAP_GATE(IRQ63, asm_irq_handler_63);
    SET_KERNEL_TRAP_GATE(IRQ64, asm_irq_handler_64);

    // Load the IDT
    install_idt();
}

__PRIVILEGED_CODE
void install_idt() {
    asm volatile ("lidt %0" :: "m"(g_kernel_idt_descriptor));
}
} // namespace arch::x86

__PRIVILEGED_CODE
void enable_interrupts() {
    asm volatile ("sti");
}

__PRIVILEGED_CODE
void disable_interrupts() {
    asm volatile ("cli");
}

static void decode_rflags(uint64_t rflags, char* buffer, size_t buffer_size) {
    sprintf(buffer, buffer_size, "[ ");
    
    // IOPL (Bits 12-13)
    uint8_t iopl = (rflags >> 12) & 0x3;
    sprintf(buffer + strlen(buffer), buffer_size - strlen(buffer), "IOPL=%u ", iopl);

    // IF (Interrupt Flag, Bit 9)
    if (rflags & (1 << 9)) {
        strncat(buffer, "IF ", buffer_size - strlen(buffer) - 1);
    }

    // TF (Trap Flag, Bit 8)
    if (rflags & (1 << 8)) {
        strncat(buffer, "TF ", buffer_size - strlen(buffer) - 1);
    }

    // DF (Direction Flag, Bit 10)
    if (rflags & (1 << 10)) {
        strncat(buffer, "DF ", buffer_size - strlen(buffer) - 1);
    }

    // OF (Overflow Flag, Bit 11)
    if (rflags & (1 << 11)) {
        strncat(buffer, "OF ", buffer_size - strlen(buffer) - 1);
    }

    // SF (Sign Flag, Bit 7)
    if (rflags & (1 << 7)) {
        strncat(buffer, "SF ", buffer_size - strlen(buffer) - 1);
    }

    // ZF (Zero Flag, Bit 6)
    if (rflags & (1 << 6)) {
        strncat(buffer, "ZF ", buffer_size - strlen(buffer) - 1);
    }

    // PF (Parity Flag, Bit 2)
    if (rflags & (1 << 2)) {
        strncat(buffer, "PF ", buffer_size - strlen(buffer) - 1);
    }

    // CF (Carry Flag, Bit 0)
    if (rflags & (1 << 0)) {
        strncat(buffer, "CF ", buffer_size - strlen(buffer) - 1);
    }

    // Close the flags representation
    strncat(buffer, "]", buffer_size - strlen(buffer) - 1);
}

bool is_valid_address(void* addr) {
    constexpr uint64_t lower_bound = 0x1000;
    constexpr uint64_t upper_bound = 0xFFFFFFFFFFFF;

    uint64_t address = (uint64_t)addr;
    return (address >= lower_bound && address <= upper_bound);
}

void print_backtrace(ptregs* regs) {
    kprint("<------- BACKTRACE ------->\n");

    uint64_t *rbp = (uint64_t*)regs->rbp;
    uint64_t rip = regs->hwframe.rip;

    // Print the current instruction pointer (RIP)
    kprint("RIP: 0x%llx\n", rip);

    // Iterate through the stack frames
    while (rbp) {
        // Validate that the pointer is in a reasonable range
        if (!is_valid_address(rbp)) {
            kprint(" [Invalid RBP: 0x%llx]\n", (uint64_t)rbp);
            break;
        }

        uint64_t next_rip = *(rbp + 1); // Next instruction pointer (return address)
        if (next_rip == 0x0)
            break;

        kprint(" -> 0x%llx\n", next_rip);

        rbp = (uint64_t*)*rbp; // Move to the next frame
    }

    kprint("\n");
}

void panic(ptregs* regs) {
    // To ensure that the kernel panic messages get printed without getting jumbled
    arch::x86::g_panic_lock.lock();

    // Decode RFLAGS
    char rflags_buffer[64] = { 0 };
    decode_rflags(regs->hwframe.rflags, rflags_buffer, sizeof(rflags_buffer));

    kprint("\n[PANIC] Kernel Panic! System Halted. CPU: %i \n", current->cpu);
    kprint("============================================================\n");

    // Prints the backtrace
    print_backtrace(regs);

    // General purpose registers
    kprint("General Purpose Registers:\n");
    kprint("  RAX: 0x%016llx   RBX: 0x%016llx\n", regs->rax, regs->rbx);
    kprint("  RCX: 0x%016llx   RDX: 0x%016llx\n", regs->rcx, regs->rdx);
    kprint("  RSI: 0x%016llx   RDI: 0x%016llx\n", regs->rsi, regs->rdi);
    kprint("  RBP: 0x%016llx   RSP: 0x%016llx\n", regs->rbp, regs->hwframe.rsp);
    kprint("  R8 : 0x%016llx   R9 : 0x%016llx\n", regs->r8, regs->r9);
    kprint("  R10: 0x%016llx   R11: 0x%016llx\n", regs->r10, regs->r11);
    kprint("  R12: 0x%016llx   R13: 0x%016llx\n", regs->r12, regs->r13);
    kprint("  R14: 0x%016llx   R15: 0x%016llx\n", regs->r14, regs->r15);

    // Segment selectors
    kprint("\nSegment Selectors:\n");
    kprint("  CS:  0x%016llx   DS:  0x%016llx\n", regs->hwframe.cs, regs->ds);
    kprint("  ES:  0x%016llx   FS:  0x%016llx\n", regs->es, regs->fs);
    kprint("  GS:  0x%016llx   SS:  0x%016llx\n", regs->gs, regs->hwframe.ss);

    // Instruction pointer, flags, and error code
    kprint("\nInstruction and Context Information:\n");
    kprint("  RIP: 0x%016llx   RFLAGS: 0x%llx %s\n",
        regs->hwframe.rip, regs->hwframe.rflags, rflags_buffer);
    kprint("  IRQ: %s\n  Error Code: 0x%0llx\n",
        arch::x86::g_cpu_exception_strings[regs->intno], regs->error);
    kprint("  CPU: %i\n", current->cpu);
    kprint("  Thread Name: '%s'\n", current->name);
    kprint("  Privilege: %s\n", current->elevated ? "elevated" : "lowered");

    // Control registers
    uint64_t cr0, cr2, cr3, cr4;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    kprint("\nControl Registers:\n");
    kprint("  CR0: 0x%016llx   CR2: 0x%016llx\n", cr0, cr2);
    kprint("  CR3: 0x%016llx   CR4: 0x%016llx\n", cr3, cr4);

    // Final separator
    kprint("============================================================\n");
    kprint("System halted.\n");

    // Release the panic lock
    arch::x86::g_panic_lock.unlock();

    while (1) {
        asm volatile("hlt");
    }
}

void panic(const char* msg) {
    kprint("%s\n", msg);
    asm volatile ("int $49");
}

__PRIVILEGED_CODE
uint8_t find_free_irq_vector() {
    for (uint8_t irqno = IRQ18; irqno < IRQ64; irqno++) {
        uint64_t irq_table_index = static_cast<uint64_t>(irqno) - IRQ0;
        irq_desc* desc = &arch::x86::g_irq_handler_table.descriptors[irq_table_index];
        if (!desc->handler) {
            return irqno;
        }
    }

    return 0;
}

__PRIVILEGED_CODE
bool register_irq_handler(uint8_t irqno, irq_handler_t handler, uint8_t flags, void* cookie) {
    uint64_t irq_table_index = static_cast<uint64_t>(irqno) - IRQ0;
    irq_desc* desc = &arch::x86::g_irq_handler_table.descriptors[irq_table_index];
    if (desc->handler && reinterpret_cast<uint64_t>(desc->handler) != RESERVED_IRQ_VECTOR) {
        kprint("[WARN] register_irq_handler(): IRQ%i handler already exists!\n", irq_table_index);
        return false;
    }

    desc->handler = handler;
    desc->flags   = flags;
    desc->cookie  = cookie;
    desc->irqno   = irqno;

    return true;
}

__PRIVILEGED_CODE
bool reserve_irq_vector(uint8_t irqno) {
    uint64_t irq_table_index = static_cast<uint64_t>(irqno) - IRQ0;
    irq_desc* desc = &arch::x86::g_irq_handler_table.descriptors[irq_table_index];
    if (desc->handler) {
        kprint("[WARN] reserve_irq_vector(): IRQ%i vector is already taken!\n", irq_table_index);
        return false;
    }

    desc->handler = reinterpret_cast<irq_handler_t>(RESERVED_IRQ_VECTOR);
    return true;
}

__PRIVILEGED_CODE
void irq_send_eoi() {
    arch::x86::lapic::get()->complete_irq();
}
#endif // ARCH_X86_64

