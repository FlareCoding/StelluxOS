#include "phys_addr_translation.h"

// The kernel would set this at initialization stage
uint64_t __kern_phys_base;

// Start of the kernel
extern uint64_t __ksymstart;

#define KERNEL_VIRTUAL_BASE reinterpret_cast<uint64_t>(&__ksymstart)

uint64_t physToVirtAddr(uint64_t paddr) {
    return paddr + (KERNEL_VIRTUAL_BASE - __kern_phys_base);
}

uint64_t virtToPhysAddr(uint64_t vaddr) {
    return vaddr - (KERNEL_VIRTUAL_BASE - __kern_phys_base);
}
