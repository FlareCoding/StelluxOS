#ifndef PROCESS_H
#define PROCESS_H
#include <interrupts/interrupts.h>

struct CpuContext {
    // General purpose registers
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

    // Stack pointer, instruction pointer, and flags register
    uint64_t rip, rsp, rflags;

    // Segment registers
    uint64_t cs, ds, es, fs, gs, ss;

    // Top level page table pointer
    uint64_t cr3;
} __attribute__((packed));

enum class ProcessState {
    NEW,        // Process created but not yet ready to run
    READY,      // Ready to be scheduled
    RUNNING,    // Currently executing
    WAITING,    // Waiting for some resource
    TERMINATED  // Finished execution
};

typedef struct ProcessControlBlock {
    CpuContext      context;
    ProcessState    state;
    uint64_t        pid;
    uint64_t        priority;
    uint64_t        kernelStack;
} __attribute__((packed)) PCB;

// Saves context registers from the interrupt frame into a CPU context struct
void saveCpuContext(CpuContext* to_save, InterruptFrame* frame);

// Saves context registers from the CPU context struct into an interrupt frame
void restoreCpuContext(CpuContext* context, InterruptFrame* frame);

// Saves and restores necessary registers into the appropriate process control blocks
void switchContext(PCB* from, PCB* to, InterruptFrame *frame);

#endif
