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
#define IRQ16  48
#define IRQ17  49
#define IRQ18  50
#define IRQ19  51
#define IRQ20  52
#define IRQ21  53
#define IRQ22  54
#define IRQ23  55
#define IRQ24  56
#define IRQ25  57
#define IRQ26  58
#define IRQ27  59
#define IRQ28  60
#define IRQ29  61
#define IRQ30  62
#define IRQ31  63
#define IRQ32  64
#define IRQ33  65
#define IRQ34  66
#define IRQ35  67
#define IRQ36  68
#define IRQ37  69
#define IRQ38  70
#define IRQ39  71
#define IRQ40  72
#define IRQ41  73
#define IRQ42  74
#define IRQ43  75
#define IRQ44  76
#define IRQ45  77
#define IRQ46  78
#define IRQ47  79
#define IRQ48  80
#define IRQ49  81
#define IRQ50  82
#define IRQ51  83
#define IRQ52  84
#define IRQ53  85
#define IRQ54  86
#define IRQ55  87
#define IRQ56  88
#define IRQ57  89
#define IRQ58  90
#define IRQ59  91
#define IRQ60  92
#define IRQ61  93
#define IRQ62  94
#define IRQ63  95
#define IRQ64  96

#define IRQ_EDGE_TRIGGERED  0
#define IRQ_LEVEL_TRIGGERED 1

struct InterruptFrame {
    uint64_t rip;           // Instruction pointer (address of the instruction that was interrupted)
    uint64_t cs;            // Code segment selector
    uint64_t rflags;        // RFLAGS register (contains status and control flags)
    uint64_t rsp;           // Stack pointer (points to the top of the stack)
    uint64_t ss;            // Stack segment selector
} __attribute__((packed));

struct PtRegs {
    // Segment selectors
    uint64_t gs;
    uint64_t fs;
    uint64_t es;
    uint64_t ds;

    // General purpose registers
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp;
    uint64_t rbx, rdx, rcx;
    uint64_t rax;
    uint64_t intno;         // Interrupt number in case of an interrupt context
    uint64_t error;         // Error code in case of a CPU exception (0 if no error code needed)
    InterruptFrame hwframe; // Hardware interrupt frame
} __attribute__((packed));

__force_inline__ void enableInterrupts() {
    __asm__ volatile("sti");
}

__force_inline__ void disableInterrupts() {
    __asm__ volatile("cli");
}

bool areInterruptsEnabled();

typedef int irqreturn_t;

typedef irqreturn_t (*IrqHandler_t)(PtRegs* ptregs, void* cookie);

#define DEFINE_INT_HANDLER(name) \
    irqreturn_t name( \
        PtRegs* ptregs, \
        void* cookie \
    )

#define IRQ_HANDLED     0
#define IRQ_UNHANDLED   1

DEFINE_INT_HANDLER(_userspace_common_exc_handler);

DEFINE_INT_HANDLER(_exc_handler_div);
DEFINE_INT_HANDLER(_exc_handler_pf);

DEFINE_INT_HANDLER(_irq_handler_timer);
DEFINE_INT_HANDLER(_irq_handler_keyboard);

DEFINE_INT_HANDLER(_irq_handler_schedule);

struct IrqDescriptor {
    // Specifies the handler function
    IrqHandler_t handler;

    // Device-specific data pointer
    void* cookie;

    // Specifies whether or not APIC eoi acknowledgement should be
    // done immediately before passing control to the handler.
    uint8_t fastApicEoi;

    // IRQ number associated with the interrupt handler
    uint8_t irqno;

    // Reserved/padding
    uint16_t rsvd;
};

uint8_t findFreeIrqVector();

bool registerIrqHandler(uint8_t irqno, IrqHandler_t handler, bool fastApicEoi, void* cookie);

void routeIoApicIrq(uint8_t irqLine, uint8_t irqno, uint8_t cpu = 0, uint8_t levelTriggered = 0);

#endif
