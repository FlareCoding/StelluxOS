#ifndef IDT_H
#define IDT_H
#include "interrupts.h"

#define MAX_IDT_ENTRIES 256

struct IdtGateDescriptor {
    uint16_t    offsetLow;
    uint16_t    selector;         // Selector into GDT
    uint8_t     ist         : 3;  // Index into an interrupt stack table [0..7]
    uint8_t     reserved0   : 5;
    union {
        struct {
            uint8_t     type        : 4;
            uint8_t     reserved1   : 1;
            uint8_t     dpl         : 2;  // Descriptor privilege level (0 - kernel  3 - user)
            uint8_t     present     : 1;
        };
        uint8_t flags;
    };
    uint16_t    offsetMid;
    uint32_t    offsetHigh;
    uint32_t    reserved2;
} __attribute__((packed));

struct IdtDescriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct InterruptDescriptorTable {
    IdtGateDescriptor entries[MAX_IDT_ENTRIES];
} __attribute__((packed));

extern IdtDescriptor g_kernelIdtDescriptor;
extern InterruptDescriptorTable g_kernelIdt;

// Constants for the Gate Descriptor Type Attribute
#define INTERRUPT_GATE   0x0E  // 64-bit interrupt gate, same as 32-bit
#define TRAP_GATE        0x0F  // 64-bit trap gate, same as 32-bit

// Constants for Descriptor Privilege Level
#define KERNEL_DPL 0  // Kernel-level
#define USER_DPL   3  // User-level

// Selector
#define KERNEL_CS 0x08  // Code segment selector for kernel, assuming it's the second entry in the GDT

// Macro to set the IDT gate
// entryIndex: Index of the IDT entry
// isr: Address of the interrupt service routine
// istIndex: Index into the Interrupt Stack Table
// type: Type of the gate (e.g., INTERRUPT_GATE_64)
// dpl: Descriptor privilege level (e.g., KERNEL_DPL)
// selector: Selector to load (e.g., KERNEL_CS)
#define SET_IDT_GATE(entryIndex, isr, istIndex, _type, _dpl, _selector)                        \
    do {                                                                                       \
        g_kernelIdt.entries[entryIndex].offsetLow  = (uint16_t)(uint64_t)(isr);                \
        g_kernelIdt.entries[entryIndex].offsetMid  = (uint16_t)((uint64_t)(isr) >> 16);        \
        g_kernelIdt.entries[entryIndex].offsetHigh = (uint32_t)((uint64_t)(isr) >> 32);        \
        g_kernelIdt.entries[entryIndex].selector   = _selector;                                \
        g_kernelIdt.entries[entryIndex].ist        = istIndex;                                 \
        g_kernelIdt.entries[entryIndex].type       = _type;                                    \
        g_kernelIdt.entries[entryIndex].dpl        = _dpl;                                     \
        g_kernelIdt.entries[entryIndex].present    = 1;                                        \
        g_kernelIdt.entries[entryIndex].reserved0  = 0;                                        \
        g_kernelIdt.entries[entryIndex].reserved1  = 0;                                        \
        g_kernelIdt.entries[entryIndex].reserved2  = 0;                                        \
    } while(0)

// Predefined macros to set commonly used gates
#define SET_KERNEL_INTERRUPT_GATE(entryIndex, isr) \
    SET_IDT_GATE(entryIndex, isr, 0, INTERRUPT_GATE, KERNEL_DPL, KERNEL_CS)

#define SET_KERNEL_TRAP_GATE(entryIndex, isr) \
    SET_IDT_GATE(entryIndex, isr, 0, TRAP_GATE, KERNEL_DPL, KERNEL_CS)

#define SET_USER_INTERRUPT_GATE(entryIndex, isr) \
    SET_IDT_GATE(entryIndex, isr, 0, INTERRUPT_GATE, USER_DPL, KERNEL_CS)

#define SET_USER_TRAP_GATE(entryIndex, isr) \
    SET_IDT_GATE(entryIndex, isr, 0, TRAP_GATE, USER_DPL, KERNEL_CS)

EXTERN_C void initializeAndInstallIdt();

#endif