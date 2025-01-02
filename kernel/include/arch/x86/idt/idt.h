#ifdef ARCH_X86_64
#ifndef IDT_H
#define IDT_H

#include <interrupts/irq.h>
#include <process/ptregs.h>

#define MAX_IDT_ENTRIES 256

// Constants for the Gate Descriptor Type Attribute
#define INTERRUPT_GATE   0x0E  // 64-bit interrupt gate, same as 32-bit
#define TRAP_GATE        0x0F  // 64-bit trap gate, same as 32-bit

// Constants for Descriptor Privilege Level
#define KERNEL_DPL 0  // Kernel-level
#define USER_DPL   3  // User-level

// Selector
#define KERNEL_CS   0x08  // Code segment selector for kernel

// Exception Interrupts
#define EXC_DIVIDE_BY_ZERO          0
#define EXC_DEBUG                   1
#define EXC_NMI                     2
#define EXC_BREAKPOINT              3
#define EXC_OVERFLOW                4
#define EXC_BOUND_RANGE             5
#define EXC_INVALID_OPCODE          6
#define EXC_DEVICE_NOT_AVAILABLE    7
#define EXC_DOUBLE_FAULT            8
#define EXC_COPROCESSOR_SEG_OVERRUN 9
#define EXC_INVALID_TSS            10
#define EXC_SEGMENT_NOT_PRESENT    11
#define EXC_STACK_FAULT            12
#define EXC_GENERAL_PROTECTION     13
#define EXC_PAGE_FAULT             14
#define EXC_RESERVED               15
#define EXC_X87_FLOATING_POINT     16
#define EXC_ALIGNMENT_CHECK        17
#define EXC_MACHINE_CHECK          18
#define EXC_SIMD_FLOATING_POINT    19
#define EXC_VIRTUALIZATION         20
#define EXC_HYPERVISOR_VIOLATION   21
#define EXC_VMM_COMMUNICATION      28
#define EXC_SECURITY_EXTENSION     29
#define EXC_SECURITY_EXCEPTION     30

namespace arch::x86 {
struct idt_gate_descriptor {
    uint16_t    offset_low;
    uint16_t    selector;         // Selector into gdt
    uint8_t     ist         : 3;  // Index into an interrupt stack table [0..7]
    uint8_t     reserved0   : 5;
    union {
        struct {
            uint8_t     type        : 4;
            uint8_t     reserved1   : 1;
            uint8_t     dpl         : 2;  // Descriptor privilege level (0 - kernel  3 - user)
            uint8_t     present     : 1;
        } __attribute__((packed));
        uint8_t flags;
    };
    uint16_t    offset_mid;
    uint32_t    offset_high;
    uint32_t    reserved2;
} __attribute__((packed));

struct idt_desc {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct interrupt_descriptor_table {
    idt_gate_descriptor entries[MAX_IDT_ENTRIES];
} __attribute__((packed));

// Macro to set the IDT gate
// entry_index: Index of the IDT entry
// isr: Address of the interrupt service routine
// ist: Index into the Interrupt Stack Table
// _type: Type of the gate (e.g., INTERRUPT_GATE_64)
// _dpl: Descriptor privilege level (e.g., KERNEL_DPL)
// _selector: Selector to load (e.g., KERNEL_CS)
#define SET_IDT_GATE(entry_index, isr, ist_index, _type, _dpl, _selector)                        \
    do {                                                                                         \
        g_kernel_idt.entries[entry_index].offset_low  = (uint16_t)(uint64_t)(isr);               \
        g_kernel_idt.entries[entry_index].offset_mid  = (uint16_t)((uint64_t)(isr) >> 16);       \
        g_kernel_idt.entries[entry_index].offset_high = (uint32_t)((uint64_t)(isr) >> 32);       \
        g_kernel_idt.entries[entry_index].selector   = _selector;                                \
        g_kernel_idt.entries[entry_index].ist        = ist_index;                                \
        g_kernel_idt.entries[entry_index].type       = _type;                                    \
        g_kernel_idt.entries[entry_index].dpl        = _dpl;                                     \
        g_kernel_idt.entries[entry_index].present    = 1;                                        \
        g_kernel_idt.entries[entry_index].reserved0  = 0;                                        \
        g_kernel_idt.entries[entry_index].reserved1  = 0;                                        \
        g_kernel_idt.entries[entry_index].reserved2  = 0;                                        \
    } while(0)

// Predefined macros to set commonly used gates
#define SET_KERNEL_INTERRUPT_GATE(entry_index, isr) \
    SET_IDT_GATE(entry_index, isr, 0, INTERRUPT_GATE, KERNEL_DPL, KERNEL_CS)

#define SET_KERNEL_TRAP_GATE(entry_index, isr) \
    SET_IDT_GATE(entry_index, isr, 0, TRAP_GATE, KERNEL_DPL, KERNEL_CS)

#define SET_USER_INTERRUPT_GATE(entry_index, isr) \
    SET_IDT_GATE(entry_index, isr, 0, INTERRUPT_GATE, USER_DPL, KERNEL_CS)

#define SET_USER_TRAP_GATE(entry_index, isr) \
    SET_IDT_GATE(entry_index, isr, 0, TRAP_GATE, USER_DPL, KERNEL_CS)

/**
 * @brief Initializes the Interrupt Descriptor Table (IDT).
 * 
 * Sets up the IDT by configuring all necessary interrupt and exception handlers.
 * 
 * @note Privilege: **required**
 */
EXTERN_C __PRIVILEGED_CODE void init_idt();

/**
 * @brief Installs the configured Interrupt Descriptor Table (IDT).
 * 
 * Loads the IDT into the processor using the `lidt` instruction, making it active
 * for handling interrupts and exceptions.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void install_idt();
} // namespace arch::x86

#endif // IDT_H
#endif // ARCH_X86_64
