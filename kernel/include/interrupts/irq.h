#ifndef IRQ_H
#define IRQ_H
#include <types.h>

#define IRQ_HANDLED     0
#define IRQ_UNHANDLED   1

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

typedef int irqreturn_t;
struct ptregs;

typedef irqreturn_t (*irq_handler_t)(ptregs* regs, void* cookie);

#define DEFINE_INT_HANDLER(name) \
    __PRIVILEGED_CODE irqreturn_t name( \
        ptregs* regs, \
        void* cookie \
    )

struct irq_desc {
    // Specifies the handler function
    irq_handler_t handler;

    // Device-specific data pointer
    void* cookie;

    union {
        uint8_t flags;

#ifdef ARCH_X86_64
        // Specifies whether or not APIC eoi acknowledgement should be
        // done immediately before passing control to the handler.
        uint8_t fast_apic_eoi; 
#endif
    };


    // IRQ number associated with the interrupt handler
    uint8_t irqno;

    // Reserved/padding
    uint16_t rsvd;
};

/**
 * @brief Enables CPU interrupts.
 */
__PRIVILEGED_CODE void enable_interrupts();

/**
 * @brief Disables CPU interrupts.
 */
__PRIVILEGED_CODE void disable_interrupts();

/**
 * @brief Handles a kernel panic by displaying register information and halting the system.
 * 
 * @param regs Pointer to the register state at the time of the panic.
 */
void panic(ptregs* regs);

__PRIVILEGED_CODE uint8_t find_free_irq_vector();

__PRIVILEGED_CODE bool register_irq_handler(uint8_t irqno, irq_handler_t handler, uint8_t flags, void* cookie);

__PRIVILEGED_CODE void route_legacy_irq(uint8_t irq_line, uint8_t irqno, uint8_t cpu = 0, uint8_t level_triggered = 0);

#endif // IRQ_H