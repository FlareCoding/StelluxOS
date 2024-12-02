#ifndef PTREGS_H
#define PTREGS_H
#include <types.h>

#ifdef ARCH_X86_64
struct interrupt_hw_frame {
    uint64_t rip;           // Instruction pointer (address of the instruction that was interrupted)
    uint64_t cs;            // Code segment selector
    uint64_t rflags;        // RFLAGS register (contains status and control flags)
    uint64_t rsp;           // Stack pointer (points to the top of the stack)
    uint64_t ss;            // Stack segment selector
} __attribute__((packed));

struct ptregs {
    // Segment selectors
    uint64_t gs;
    uint64_t fs;
    uint64_t es;
    uint64_t ds;

    // General purpose registers
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp;
    uint64_t rbx, rdx, rcx;
    uint64_t rax;
    uint64_t intno;             // Interrupt number in case of an interrupt context
    uint64_t error;             // Error code in case of a CPU exception (0 if no error code needed)
    interrupt_hw_frame hwframe; // Hardware interrupt frame
} __attribute__((packed));
#else
struct ptregs {};
#endif

#endif // PTREGS_H
