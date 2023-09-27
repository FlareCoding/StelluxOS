#ifndef MSR_H
#define MSR_H
#include <ktypes.h>

uint64_t readMsr(
    uint32_t msr
);

void writeMsr(
    uint32_t msr,
    uint64_t value
);

#endif
