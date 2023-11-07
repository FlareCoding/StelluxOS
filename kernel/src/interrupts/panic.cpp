#include "panic.h"
#include <kprint.h>

void kpanic(PtRegs* frame) {
    uint64_t cr0, cr2, cr3, cr4;

    // Disable interrupts
    disableInterrupts();

    // Read the control registers using inline assembly
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

    kprintChar('\n');
    kprintError("====== PANIC: CPU EXCEPTION ======\n");
    kprintInfo("Interrupt Number: %llx\n", frame->intno);
    kprintInfo("Error Code: %llx\n", frame->error);
    
    kprint("======= REGISTER STATE =======\n");

    // Display registers in rows of 3
    kprintInfo("RAX: %llx  RCX: %llx  RDX: %llx\n", frame->rax, frame->rcx, frame->rdx);
    kprintInfo("RBX: %llx  RSP: %llx  RBP: %llx\n", frame->rbx, frame->hwframe.rsp, frame->rbp);
    kprintInfo("RSI: %llx  RDI: %llx  R8 : %llx\n", frame->rsi, frame->rdi, frame->r8);
    kprintInfo("R9 : %llx  R10: %llx  R11: %llx\n", frame->r9, frame->r10, frame->r11);
    kprintInfo("R12: %llx  R13: %llx  R14: %llx\n", frame->r12, frame->r13, frame->r14);
    kprintInfo("R15: %llx\n", frame->r15);

    kprint("======= SEGMENT REGISTERS =======\n");
    kprintInfo("CS : %llx  DS : %llx  ES : %llx\n", frame->hwframe.cs, frame->ds, frame->es);
    kprintInfo("FS : %llx  GS : %llx  SS : %llx\n", frame->fs, frame->gs, frame->hwframe.ss);

    kprint("======= CONTROL REGISTERS =======\n");
    kprintInfo("CR0: %llx  CR2: %llx  CR3: %llx  CR4: %llx\n", cr0, cr2, cr3, cr4);

    kprint("======= SPECIAL REGISTERS =======\n");
    kprintInfo("RIP: %llx  RFLAGS: %llx\n", frame->hwframe.rip, frame->hwframe.rflags);

    kprintError("======= SYSTEM HALTED =======\n");
    for (;;) {
        // Loop indefinitely to halt the system
        __asm__ volatile ("hlt");
    }
}
