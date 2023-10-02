#include "process.h"
#include <paging/page.h>
#include <gdt/gdt.h>

void saveCpuContext(CpuContext* context, InterruptFrame* frame) {
    context->rax = frame->rax;
    context->rbx = frame->rbx;
    context->rcx = frame->rcx;
    context->rdx = frame->rdx;
    context->rsi = frame->rsi;
    context->rdi = frame->rdi;
    context->rbp = frame->rbp;
    context->r8 = frame->r8;
    context->r9 = frame->r9;
    context->r10 = frame->r10;
    context->r11 = frame->r11;
    context->r12 = frame->r12;
    context->r13 = frame->r13;
    context->r14 = frame->r14;
    context->r15 = frame->r15;
    context->rip = frame->rip;
    context->rsp = frame->rsp;
    context->rflags = frame->rflags;
    context->cs = frame->cs;
    context->ss = frame->ss;
    context->ds = frame->ds;
    context->es = frame->es;
    context->fs = frame->fs;
    context->gs = frame->gs;

    // Read top level page table pointer from cr3
    context->cr3 = reinterpret_cast<uint64_t>(paging::getCurrentTopLevelPageTable());
}

void restoreCpuContext(CpuContext* context, InterruptFrame* frame) {
    frame->rax = context->rax;
    frame->rbx = context->rbx;
    frame->rcx = context->rcx;
    frame->rdx = context->rdx;
    frame->rsi = context->rsi;
    frame->rdi = context->rdi;
    frame->rbp = context->rbp;
    frame->r8 = context->r8;
    frame->r9 = context->r9;
    frame->r10 = context->r10;
    frame->r11 = context->r11;
    frame->r12 = context->r12;
    frame->r13 = context->r13;
    frame->r14 = context->r14;
    frame->r15 = context->r15;
    frame->rip = context->rip;
    frame->rsp = context->rsp;
    frame->rflags = context->rflags;
    frame->cs = context->cs;
    frame->ss = context->ss;
    frame->ds = context->ds;
    frame->es = context->es;
    frame->fs = context->fs;
    frame->gs = context->gs;

    // Read top level page table pointer from cr3
    paging::setCurrentTopLevelPageTable(reinterpret_cast<paging::PageTable*>(context->cr3));
}

void switchContext(PCB* from, PCB* to, InterruptFrame *frame) {
    // Get the pointer to the active TSS
    TaskStateSegment* tss = getActiveTSS();
    
    // Save the current context into the 'from' PCB
    saveCpuContext(&from->context, frame);

    // Save the current kernel stack
    from->kernelStack = tss->rsp0;

    // Update the state of the processes
    from->state = ProcessState::READY;
    to->state = ProcessState::RUNNING;

    // Set the new kernel stack
    tss->rsp0 = to->kernelStack;

    // Restore the context from the 'to' PCB
    restoreCpuContext(&to->context, frame);
}
