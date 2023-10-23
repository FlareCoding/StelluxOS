#ifndef MSR_H
#define MSR_H
#include <ktypes.h>

#define IA32_EFER       0xC0000080
#define IA32_EFER_SCE   0x00000001
#define IA32_STAR       0xC0000081
#define IA32_LSTAR      0xC0000082
#define IA32_FMASK      0xC0000084

#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

__PRIVILEGED_CODE
uint64_t readMsr(
    uint32_t msr
);

__PRIVILEGED_CODE
void writeMsr(
    uint32_t msr,
    uint64_t value
);

void enableSyscallInterface();

#endif
