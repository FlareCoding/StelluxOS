#include "interrupts.h"
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
    disableInterrupts();

    kprintColoredEx("#DIV", TEXT_COLOR_RED);
    kprintFmtColored(TEXT_COLOR_WHITE, " faulting instruction at 0x%llx\n", frame->rip);
    kprintColoredEx("#DIV ", TEXT_COLOR_RED);
    kprintColoredEx("Your goomba code tried to divide by 0\n", TEXT_COLOR_WHITE);

    kprintError("This is a stub for a panic screen!\n");
    while (true);
}

DEFINE_INT_HANDLER(_exc_handler_pf) {
    disableInterrupts();

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
    kprintChar('\n');

    kprintError("This is a stub for a panic screen!\n");
    while (true);
}

DEFINE_INT_HANDLER(_irq_handler_timer) {
    (void)frame;

    static uint64_t count = 0;
    ++count;

    kprintFmtColored(TEXT_COLOR_YELLOW, "Timer went off! Time: %lli\r", count);
}

