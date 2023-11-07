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
} __attribute__((packed));

enum class ProcessState {
    INVALID = 0, // Process doesn't exist
    NEW,        // Process created but not yet ready to run
    READY,      // Ready to be scheduled
    RUNNING,    // Currently executing
    WAITING,    // Waiting for some resource
    TERMINATED  // Finished execution
};

typedef struct ProcessControlBlock {
    CpuContext      context;
    ProcessState    state;
    int64_t         pid;
    uint64_t        priority;
    uint64_t        cr3;
    uint64_t        kernelStack;
    int8_t          elevated;
} PCB;

typedef int64_t pid_t;

// Saves context registers from the interrupt frame into a CPU context struct
void saveCpuContext(CpuContext* to_save, PtRegs* frame);

// Saves context registers from the CPU context struct into an interrupt frame
void restoreCpuContext(CpuContext* context, PtRegs* frame);

// Saves and restores necessary registers into the appropriate
// process control blocks using an interrupt frame.
// *Note* Meant to be called from within an interrupt handler
// and context would get switched upon interrupt return.
void switchContextInIrq(PCB* from, PCB* to, PtRegs *frame);

//
// More low level context switch that only switches the CPU context in-place.
//
void switchTo(PCB* from, PCB* to);

#endif
