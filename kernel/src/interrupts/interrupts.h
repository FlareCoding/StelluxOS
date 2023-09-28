#ifndef INTERRUPTS_H
#define INTERRUPTS_H
#include <ktypes.h>

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

// IRQ Hardware Interrupts (PIC)
#define IRQ_PIC_TIMER              32
#define IRQ_KEYBOARD               33
#define IRQ_SLAVE_PIC              34
#define IRQ_COM2                   35
#define IRQ_COM1                   36
#define IRQ_LPT2                   37
#define IRQ_FLOPPY_DISK            38
#define IRQ_LPT1                   39
#define IRQ_RTC                    40
#define IRQ_PERIPHERALS            41
#define IRQ_PRIMARY_ATA            42
#define IRQ_SECONDARY_ATA          43
#define IRQ_KEYBOARD_CONTROLLER    44
#define IRQ_FPU                    45
#define IRQ_HARDDISK               46
#define IRQ_ACPI                   47

// Generalized IRQs
#define IRQ0   32
#define IRQ1   33
#define IRQ2   34
#define IRQ3   35
#define IRQ4   36
#define IRQ5   37
#define IRQ6   38
#define IRQ7   39
#define IRQ8   40
#define IRQ9   41
#define IRQ10  42
#define IRQ11  43
#define IRQ12  44
#define IRQ13  45
#define IRQ14  46
#define IRQ15  47

// Additional Software Interrupts can be defined here (INT)
// ...

struct InterruptFrame {
    uint64_t ds;            // Data segment selector

    // General purpose registers
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp;
    uint64_t rbx, rdx, rcx;
    uint64_t rax;
    uint64_t intno;         // Interrupt number
    uint64_t error;         // Error code (0 if no error code needed)
    uint64_t rip;           // Instruction pointer (address of the instruction that was interrupted)
    uint64_t cs;            // Code segment selector
    uint64_t rflags;        // RFLAGS register (contains status and control flags)
    uint64_t rsp;           // Stack pointer (points to the top of the stack)
    uint64_t ss;            // Stack segment selector
} __attribute__((packed));

void enableInterrupts();
void disableInterrupts();

typedef void (*InterruptHandler_t)(struct InterruptFrame* frame);

#define DEFINE_INT_HANDLER(name) \
    void name( \
        struct InterruptFrame* frame \
    )

DEFINE_INT_HANDLER(_exc_handler_div);
DEFINE_INT_HANDLER(_exc_handler_pf);

DEFINE_INT_HANDLER(_irq_handler_timer);

#endif
