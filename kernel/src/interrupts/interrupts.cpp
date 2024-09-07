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
    kpanic(frame);
}

DEFINE_INT_HANDLER(_exc_handler_div) {
    acquireSpinlock(&__kexc_log_lock);

    kprintColoredEx("#DIV", TEXT_COLOR_RED);
    kprintFmtColored(TEXT_COLOR_WHITE, " faulting instruction at 0x%llx\n", frame->hwframe.rip);
    kprintColoredEx("#DIV ", TEXT_COLOR_RED);
    kprintColoredEx("Your goomba code tried to divide by 0\n", TEXT_COLOR_WHITE);

    releaseSpinlock(&__kexc_log_lock);
    kpanic(frame);
}

DEFINE_INT_HANDLER(_exc_handler_pf) {
    acquireSpinlock(&__kexc_log_lock);

    kprintColoredEx("#PF", TEXT_COLOR_RED);
    kprintFmtColored(TEXT_COLOR_WHITE, " faulting instruction at 0x%llx\n", frame->hwframe.rip);
    kprintColoredEx("#PF", TEXT_COLOR_RED);
    kprintFmtColored(TEXT_COLOR_WHITE, " error_code: (0x%llx)", frame->error);

    if (frame->error & PF_PRESENT) {
        kprintFmtColored(TEXT_COLOR_WHITE, " - page-level protection violation");
    } else {
        kprintFmtColored(TEXT_COLOR_WHITE, " - page not present");
    }

    if (frame->error & PF_WRITE) {
        kprintFmtColored(TEXT_COLOR_WHITE, " - write operation");
    } else {
        kprintFmtColored(TEXT_COLOR_WHITE, " - read operation");
    }

    if (frame->error & PF_USER) {
        if (frame->ds & 3)
            kprintFmtColored(TEXT_COLOR_WHITE, " - occurred in user mode");
        else
            kprintFmtColored(TEXT_COLOR_WHITE, " - occurred in lowered-supervisor mode");
    } else {
        if (frame->ds & 3)
            kprintFmtColored(TEXT_COLOR_WHITE, " - occurred in user-elevated mode");
        else
            kprintFmtColored(TEXT_COLOR_WHITE, " - occurred in supervisor mode");
    }
    
    kprintChar('\n');

    uint64_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    kprintWarn("Faulting address: 0x%llx\n", cr2);

    kprintChar('\n');

    releaseSpinlock(&__kexc_log_lock);
    kpanic(frame);
}

DEFINE_INT_HANDLER(_irq_handler_timer) {
    Apic::__irqGetLocalApic()->completeIrq();

    auto& sched = RRScheduler::get();
    size_t cpu = current->cpu;

    PCB* prevTask = sched.getCurrentTask(cpu);
    PCB* nextTask = sched.peekNextTask(cpu);

    if (nextTask && prevTask != nextTask) {
        // Switch the CPU context
        switchContextInIrq(cpu, cpu, prevTask, nextTask, frame);
        
        // Tell the scheduler that the context switch has been accepted
        sched.scheduleNextTask(cpu);
    }
}

DEFINE_INT_HANDLER(_irq_handler_keyboard) {
    (void)frame;

    Apic::__irqGetLocalApic()->completeIrq();
    uint8_t scancode = inByte(0x60);

    kprint("Scancode: %i\n", (int)scancode);
}
