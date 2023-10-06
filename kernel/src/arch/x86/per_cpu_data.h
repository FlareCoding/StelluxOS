#ifndef PER_CPU_DATA_H
#define PER_CPU_DATA_H
#include <process/process.h>

#define MAX_CPUS 64

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

#define current __per_cpu_data.__cpu[0].currentTask

#endif
