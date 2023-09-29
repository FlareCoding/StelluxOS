#include "interrupts.h"
#include <arch/x86/apic.h>
#include <sched/sched.h>
#include <paging/tlb.h>
#include <kprint.h>

#define PF_PRESENT  0x1  // Bit 0
#define PF_WRITE    0x2  // Bit 1
#define PF_USER     0x4  // Bit 2

void enableInterrupts() {
    __asm__ volatile("sti");
}

void disableInterrupts() {
    __asm__ volatile("cli");
}

DEFINE_INT_HANDLER(_exc_handler_div) {
    kprintColoredEx("#DIV", TEXT_COLOR_RED);
    kprintFmtColored(TEXT_COLOR_WHITE, " faulting instruction at 0x%llx\n", frame->rip);
    kprintColoredEx("#DIV ", TEXT_COLOR_RED);
    kprintColoredEx("Your goomba code tried to divide by 0\n", TEXT_COLOR_WHITE);

    kprintError("This is a stub for a panic screen!\n");
    while (true);
}

DEFINE_INT_HANDLER(_exc_handler_pf) {
    kprintColoredEx("#PF", TEXT_COLOR_RED);
    kprintFmtColored(TEXT_COLOR_WHITE, " faulting instruction at 0x%llx\n", frame->rip);
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
        kprintFmtColored(TEXT_COLOR_WHITE, " - occurred in user mode");
    } else {
        kprintFmtColored(TEXT_COLOR_WHITE, " - occurred in supervisor mode");
    }
    
    kprintChar('\n');

    uint64_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    kprintWarn("Faulting address: 0x%llx\n", cr2);

    kprintChar('\n');
    kprintError("This is a stub for a panic screen!\n");
    while (true);
}

DEFINE_INT_HANDLER(_irq_handler_timer) {
    static uint64_t count = 0;
    ++count;

    if (count % 100 == 0) {
        auto& sched = Scheduler::get();
        PCB* prevTask = sched.getCurrentTask();
        PCB* nextTask = sched.getNextTask();

        if (nextTask) {
            switchContext(prevTask, nextTask, frame);
            // kprintInfo("PID:%llu DESCHEDULED\n", prevTask->pid);
            // kprintInfo("PID:%llu SCHEDULED\n", nextTask->pid);
        }
    }

    completeApicIrq(); // ack in the beginning
    enableInterrupts();
}
