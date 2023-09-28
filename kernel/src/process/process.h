#ifndef PROCESS_H
#define PROCESS_H
#include <interrupts/interrupts.h>

struct CpuContext {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rsp;
    uint64_t rflags;
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
} __attribute__((packed)) PCB;

// Saves context registers from the interrupt frame into a CPU context struct
void saveCpuContext(CpuContext* to_save, InterruptFrame* frame);

// Saves context registers from the CPU context struct into an interrupt frame
void restoreCpuContext(CpuContext* context, InterruptFrame* frame);

// Saves and restores necessary registers into the appropriate process control blocks
void switchContext(PCB* from, PCB* to, InterruptFrame *frame);

#endif
