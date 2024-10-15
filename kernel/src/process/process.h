#ifndef PROCESS_H
#define PROCESS_H
#include <interrupts/interrupts.h>

#define MAX_PROCESS_NAME_LEN 255

typedef int64_t pid_t;

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
    pid_t           pid;
    uint64_t        priority;
    uint64_t        cr3;
    uint64_t        kernelStack;
    uint64_t        userStackTop;
    uint64_t        usergs;

    struct {
        uint64_t    elevated    : 1;
        uint64_t    cpu         : 8;
        uint64_t    flrsvd      : 55;
    } __attribute__((packed));

    char            name[MAX_PROCESS_NAME_LEN + 1];
} PCB;

using Task = PCB;

// Saves context registers from the interrupt frame into a CPU context struct
void saveCpuContext(CpuContext* context, PtRegs* frame);

// Saves context registers from the CPU context struct into an interrupt frame
void restoreCpuContext(CpuContext* context, PtRegs* frame);

// Saves and restores necessary registers into the appropriate
// process control blocks using an interrupt frame.
// *Note* Meant to be called from within an interrupt handler
// and context would get switched upon interrupt return.
void switchContextInIrq(int oldCpu, int newCpu, PCB* from, PCB* to, PtRegs *frame);

// When exiting a kernel thread, we don't care about the existing context
// since it will be purged. This routine will just load the new context
// into the provided PtRegs struct and use it to perform an 'iretq' jump. 
void exitAndSwitchCurrentContext(int cpu, PCB* newCtx, PtRegs* regs);

// Reads the current task's CPU field
uint8_t getCurrentCpuId();

#endif
