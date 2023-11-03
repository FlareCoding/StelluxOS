#include "interrupts.h"
#include <arch/x86/apic.h>
#include <sched/sched.h>
#include <paging/tlb.h>
#include <kprint.h>
#include <kstring.h>
#include <memory/kmemory.h>
#include <ports/serial.h>
#include "panic.h"

#define PF_PRESENT  0x1  // Bit 0
#define PF_WRITE    0x2  // Bit 1
#define PF_USER     0x4  // Bit 2

uint64_t _g_system_tick_count = 0;

bool areInterruptsEnabled() {
    unsigned long eflags;
    asm volatile("pushf\n""pop %0": "=rm"(eflags));

    return (eflags >> 9) & 1;
}

DEFINE_INT_HANDLER(_userspace_common_exc_handler) {
    (void)frame;

    while (1) {
        asm volatile ("hlt");
    }

    // kprintWarn("Faulting instruction: 0x%llx\n", frame->hwframe.rip);
    // kpanic(frame);
}

DEFINE_INT_HANDLER(_exc_handler_div) {
    kprintColoredEx("#DIV", TEXT_COLOR_RED);
    kprintFmtColored(TEXT_COLOR_WHITE, " faulting instruction at 0x%llx\n", frame->hwframe.rip);
    kprintColoredEx("#DIV ", TEXT_COLOR_RED);
    kprintColoredEx("Your goomba code tried to divide by 0\n", TEXT_COLOR_WHITE);

    kpanic(frame);
}

DEFINE_INT_HANDLER(_exc_handler_pf) {
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
    kpanic(frame);
}

DEFINE_INT_HANDLER(_irq_handler_timer) {
    completeApicIrq();

    ++_g_system_tick_count;

    if (_g_system_tick_count % 100 == 0) {
        auto& sched = Scheduler::get();
        PCB* prevTask = sched.getCurrentTask();
        PCB* nextTask = sched.getNextTask();

        //if (nextTask && prevTask != nextTask) {
        if (true) {
            // Update the state of the processes
            prevTask->state = ProcessState::READY;
            nextTask->state = ProcessState::RUNNING;

            // if (nextTask->context.rip == prevTask->context.rip) {
            //     writeToSerialPort(SERIAL_PORT_BASE_COM2, "MEMORY CORRUPTION CAUGHT\n");
            // }

            //char buf[256] = { 0 };
            //itoa(_g_system_tick_count, buf, 256);
            //writeToSerialPort(SERIAL_PORT_BASE_COM2, "Switching task at tick ");
            //writeToSerialPort(SERIAL_PORT_BASE_COM2, buf);
            //writeToSerialPort(SERIAL_PORT_BASE_COM2, ' ');

            // memset(buf, 0, 256);
            // htoa(prevTask->context.rip, buf, 256);
            // writeToSerialPort(SERIAL_PORT_BASE_COM2, buf);
            // writeToSerialPort(SERIAL_PORT_BASE_COM2, "-->");

            // memset(buf, 0, 256);
            // htoa(nextTask->context.rip, buf, 256);
            // writeToSerialPort(SERIAL_PORT_BASE_COM2, buf);
            // writeToSerialPort(SERIAL_PORT_BASE_COM2, "  frame->rip:");

            // memset(buf, 0, 256);
            // htoa(frame->hwframe.rip, buf, 256);
            // writeToSerialPort(SERIAL_PORT_BASE_COM2, buf);
            // writeToSerialPort(SERIAL_PORT_BASE_COM2, "\n");

            //switchContextInIrq(prevTask, nextTask, frame);
            (void)frame;
        }
    }

    enableInterrupts();
}
