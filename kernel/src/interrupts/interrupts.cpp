#include "interrupts.h"
#include <arch/x86/apic.h>
#include <sched/sched.h>
#include <paging/tlb.h>
#include <kprint.h>
#include <kstring.h>
#include <memory/kmemory.h>
#include <ports/serial.h>
#include "panic.h"
#include <sync.h>
#include <drivers/usb/xhci/xhci.h>

#define PF_PRESENT  0x1  // Bit 0
#define PF_WRITE    0x2  // Bit 1
#define PF_USER     0x4  // Bit 2

DECLARE_SPINLOCK(__kexc_log_lock);

bool areInterruptsEnabled() {
    unsigned long eflags;
    asm volatile("pushf\n""pop %0": "=rm"(eflags));

    return (eflags >> 9) & 1;
}

DEFINE_INT_HANDLER(_userspace_common_exc_handler) {
    __unused cookie;

    kpanic(ptregs);

    return IRQ_HANDLED;
}

DEFINE_INT_HANDLER(_exc_handler_div) {
    __unused cookie;

    acquireSpinlock(&__kexc_log_lock);

    dbgPrint("#DIV");
    dbgPrint(" faulting instruction at 0x%llx\n", ptregs->hwframe.rip);
    dbgPrint("#DIV ");
    dbgPrint("Your goomba code tried to divide by 0\n");

    releaseSpinlock(&__kexc_log_lock);
    kpanic(ptregs);

    return IRQ_HANDLED;
}

DEFINE_INT_HANDLER(_exc_handler_pf) {
    __unused cookie;

    acquireSpinlock(&__kexc_log_lock);

    dbgPrint("#PF");
    dbgPrint(" faulting instruction at 0x%llx\n", ptregs->hwframe.rip);
    dbgPrint("#PF");
    dbgPrint(" error_code: (0x%llx)", ptregs->error);

    if (ptregs->error & PF_PRESENT) {
        dbgPrint(" - page-level protection violation");
    } else {
        dbgPrint(" - page not present");
    }

    if (ptregs->error & PF_WRITE) {
        dbgPrint(" - write operation");
    } else {
        dbgPrint(" - read operation");
    }

    if (ptregs->error & PF_USER) {
        if (ptregs->ds & 3)
            dbgPrint(" - occurred in user mode");
        else
            dbgPrint(" - occurred in lowered-supervisor mode");
    } else {
        if (ptregs->ds & 3)
            dbgPrint(" - occurred in user-elevated mode");
        else
            dbgPrint(" - occurred in supervisor mode");
    }
    
    dbgPrint("\n");

    uint64_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    dbgPrint("Faulting address: 0x%llx\n", cr2);

    dbgPrint("\n");

    releaseSpinlock(&__kexc_log_lock);
    kpanic(ptregs);

    return IRQ_HANDLED;
}

DEFINE_INT_HANDLER(_irq_handler_timer) {
    __unused cookie;

    auto& sched = Scheduler::get();
    sched.__schedule(ptregs);

    return IRQ_HANDLED;
}

DEFINE_INT_HANDLER(_irq_handler_schedule) {
    __unused cookie;
    
    auto& sched = Scheduler::get();
    sched.__schedule(ptregs);

    return IRQ_HANDLED;
}

DEFINE_INT_HANDLER(_irq_handler_keyboard) {
    __unused ptregs;
    __unused cookie;
    uint8_t scancode = inByte(0x60);

    dbgPrint("Scancode: %i\n", (int)scancode);

    return IRQ_HANDLED;
}
