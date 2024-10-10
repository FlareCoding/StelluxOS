#include "panic.h"
#include <sched/sched.h>
#include <kprint.h>
#include <sync.h>

const char* g_cpuExceptionMessages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",

    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",

    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",

    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception"
};

DECLARE_SPINLOCK(__kpanic_lock);

void printBacktrace(PtRegs* regs) {
    dbgPrint("======= BACKTRACE =======\n");

    uint64_t *rbp = (uint64_t*)regs->rbp;
    uint64_t rip = regs->hwframe.rip;

    // Print the current instruction pointer (RIP)
    dbgPrint("RIP: 0x%llx\n", rip);

    // Iterate through the stack frames
    while (rbp) {
        uint64_t next_rip = *(rbp + 1); // Next instruction pointer (return address)
        if (next_rip == 0x0)
            break;

        dbgPrint(" -> 0x%llx\n", next_rip);

        rbp = (uint64_t*)*rbp; // Move to the next frame
    }
}

void kpanic(PtRegs* frame) {
    uint64_t cr0, cr2, cr3, cr4;

    // Disable interrupts
    disableInterrupts();

    acquireSpinlock(&__kpanic_lock);

    // Read the control registers using inline assembly
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

    dbgPrint("\n");
    dbgPrint("====== PANIC: CPU EXCEPTION %s ======\n", g_cpuExceptionMessages[frame->intno]);
    dbgPrint("CPU: %i\n", current->cpu);
    dbgPrint("Error Code: %llx\n", frame->error);

    printBacktrace(frame);
    
    dbgPrint("======= REGISTER STATE =======\n");

    // Display registers in rows of 3
    dbgPrint("RAX: %llx  RCX: %llx  RDX: %llx\n", frame->rax, frame->rcx, frame->rdx);
    dbgPrint("RBX: %llx  RSP: %llx  RBP: %llx\n", frame->rbx, frame->hwframe.rsp, frame->rbp);
    dbgPrint("RSI: %llx  RDI: %llx  R8 : %llx\n", frame->rsi, frame->rdi, frame->r8);
    dbgPrint("R9 : %llx  R10: %llx  R11: %llx\n", frame->r9, frame->r10, frame->r11);
    dbgPrint("R12: %llx  R13: %llx  R14: %llx\n", frame->r12, frame->r13, frame->r14);
    dbgPrint("R15: %llx\n", frame->r15);

    dbgPrint("======= SEGMENT REGISTERS =======\n");
    dbgPrint("CS : %llx  DS : %llx  ES : %llx\n", frame->hwframe.cs, frame->ds, frame->es);
    dbgPrint("FS : %llx  GS : %llx  SS : %llx\n", frame->fs, frame->gs, frame->hwframe.ss);

    dbgPrint("======= CONTROL REGISTERS =======\n");
    dbgPrint("CR0: %llx  CR2: %llx  CR3: %llx  CR4: %llx\n", cr0, cr2, cr3, cr4);

    dbgPrint("======= SPECIAL REGISTERS =======\n");
    dbgPrint("RIP: %llx  RFLAGS: %llx\n", frame->hwframe.rip, frame->hwframe.rflags);

    dbgPrint("======= PROCESSOR HALTED =======\n");
    releaseSpinlock(&__kpanic_lock);
    for (;;) {
        // Loop indefinitely to halt the system
        __asm__ volatile ("hlt");
    }
}
