#include "phys_addr_translation.h"

// The kernel would set this at initialization stage
uint64_t __kern_phys_base;

// Start of the kernel
extern uint64_t __ksymstart;

#define KERNEL_VIRTUAL_BASE reinterpret_cast<uint64_t>(&__ksymstart)

void* physToVirtAddr(void* paddr) {
    return reinterpret_cast<uint8_t*>(paddr) + (KERNEL_VIRTUAL_BASE - __kern_phys_base);
}

void* virtToPhysAddr(void* vaddr) {
    return reinterpret_cast<uint8_t*>(vaddr) - (KERNEL_VIRTUAL_BASE - __kern_phys_base);
}
