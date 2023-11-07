#ifndef GSFSBASE_H
#define GSFSBASE_H
#include <ktypes.h>

#define CR4_FSGSBASE_BIT 16

__PRIVILEGED_CODE
void enableFSGSBase() {
    uint64_t cr4;

    // Read the current CR4 value into cr4
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    // Set the FSGSBASE bit
    cr4 |= (1ULL << CR4_FSGSBASE_BIT);

    // Write the new CR4 value
    asm volatile ("mov %0, %%cr4" : : "r"(cr4));
}

#endif
