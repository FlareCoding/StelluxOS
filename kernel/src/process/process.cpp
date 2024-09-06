#include "process.h"
#include <paging/page.h>
#include <arch/x86/per_cpu_data.h>
#include <sched/sched.h>
#include <kelevate/kelevate.h>

EXTERN_C void __asm_ctx_switch_no_irq(PtRegs* regs);

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
    // Save the current context into the 'from' PCB
    saveCpuContext(&from->context, frame);

    // Read top level page table pointer from cr3
    from->cr3 = reinterpret_cast<uint64_t>(paging::getCurrentTopLevelPageTable());

    // Save the current kernel stack
    from->kernelStack = __per_cpu_data.__cpu[BSP_CPU_ID].currentKernelStack;

    // Set the new kernel stack
    __per_cpu_data.__cpu[BSP_CPU_ID].currentKernelStack = to->kernelStack;

    // Restore the context from the 'to' PCB
    restoreCpuContext(&to->context, frame);

    // Read top level page table pointer from cr3
    paging::setCurrentTopLevelPageTable(reinterpret_cast<paging::PageTable*>(to->cr3));

    // Set the new value of currentTask
    __per_cpu_data.__cpu[BSP_CPU_ID].currentTask = to;
}

void exitAndSwitchCurrentContext(PCB* newCtx, PtRegs* regs) {
    // Set the new kernel stack
    __per_cpu_data.__cpu[BSP_CPU_ID].currentKernelStack = newCtx->kernelStack;

    // Restore the context from the 'to' PCB
    restoreCpuContext(&newCtx->context, regs);

    // Read top level page table pointer from cr3
    paging::setCurrentTopLevelPageTable(reinterpret_cast<paging::PageTable*>(newCtx->cr3));

    // Set the new value of currentTask
    __per_cpu_data.__cpu[BSP_CPU_ID].currentTask = newCtx;

    // This will result in an 'iretq' jump instruction
    __asm_ctx_switch_no_irq(regs);
}

// Reads the current task's CPU field
uint8_t getCurrentCpuId() {
    uint8_t cpu = 0;
    RUN_ELEVATED({
        cpu = current->cpu;
    });
    return cpu;
}
