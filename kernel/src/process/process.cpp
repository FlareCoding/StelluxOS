#include "process.h"
#include <paging/page.h>
#include <gdt/gdt.h>

EXTERN_C void __asm_switch_cpu_context_and_iret(CpuContext* ctx);

void saveCpuContext(CpuContext* context, PtRegs* frame) {
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
    context->rip = frame->hwframe.rip;
    context->rsp = frame->hwframe.rsp;
    context->rflags = frame->hwframe.rflags;
    context->cs = frame->hwframe.cs;
    context->ss = frame->hwframe.ss;
    context->ds = frame->ds;
    context->es = frame->es;
    context->fs = frame->fs;
    context->gs = frame->gs;
}

void restoreCpuContext(CpuContext* context, PtRegs* frame) {
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
    frame->hwframe.rip = context->rip;
    frame->hwframe.rsp = context->rsp;
    frame->hwframe.rflags = context->rflags;
    frame->hwframe.cs = context->cs;
    frame->hwframe.ss = context->ss;
    frame->ds = context->ds;
    frame->es = context->es;
    frame->fs = context->fs;
    frame->gs = context->gs;
}

// Saves and restores necessary registers into the appropriate
// process control blocks using an interrupt frame.
// *Note* Meant to be called from within an interrupt handler
// and context would get switched upon interrupt return.
void switchContextInIrq(PCB* from, PCB* to, PtRegs* frame) {
    // Get the pointer to the active TSS
    TaskStateSegment* tss = getActiveTSS();
    
    // Save the current context into the 'from' PCB
    saveCpuContext(&from->context, frame);

    // Read top level page table pointer from cr3
    from->cr3 = reinterpret_cast<uint64_t>(paging::getCurrentTopLevelPageTable());

    // Save the current kernel stack
    from->kernelStack = tss->rsp0;

    // Update the state of the processes
    from->state = ProcessState::READY;
    to->state = ProcessState::RUNNING;

    // Set the new kernel stack
    tss->rsp0 = to->kernelStack;

    // Restore the context from the 'to' PCB
    restoreCpuContext(&to->context, frame);

    // Read top level page table pointer from cr3
    paging::setCurrentTopLevelPageTable(reinterpret_cast<paging::PageTable*>(to->cr3));
}

//
// More low level context switch that only switches the CPU context in-place.
//
void switchTo(PCB* from, PCB* to) {
    // Get the pointer to the active TSS
    TaskStateSegment* tss = getActiveTSS();

    // Read top level page table pointer from cr3
    from->cr3 = reinterpret_cast<uint64_t>(paging::getCurrentTopLevelPageTable());

    // Save the current kernel stack
    from->kernelStack = tss->rsp0;

    //
    // =================== TO-DO ===================
    //   implement saving the current cpu context
    //

    // Set the new kernel stack
    tss->rsp0 = to->kernelStack;

    // Read top level page table pointer from cr3
    paging::setCurrentTopLevelPageTable(reinterpret_cast<paging::PageTable*>(to->cr3));

    // Switch to the new context and jump to it using iret
    __asm_switch_cpu_context_and_iret(&to->context);
}
