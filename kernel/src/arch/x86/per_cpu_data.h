#ifndef PER_CPU_DATA_H
#define PER_CPU_DATA_H
#include <process/process.h>

#define MAX_CPUS 64

#define BSP_CPU_ID 0

struct CpuData {
    PCB* currentTask;               // 0x00
    uint64_t defaultKernelStack;    // 0x08
    uint64_t currentKernelStack;    // 0x10
    uint64_t currentUserStack;      // 0x18
} __attribute__((packed));

struct PerCpuData {
    CpuData __cpu[MAX_CPUS];
} __attribute__((packed));

EXTERN_C PerCpuData __per_cpu_data;

static __attribute__((always_inline)) inline PCB* getCurrentTask() {
    PCB* currentTask = nullptr;
    asm volatile (
        "movq %%gs:0x0, %0"
        : "=r" (currentTask)
    );
    return currentTask;
}

#define current getCurrentTask()

#endif
