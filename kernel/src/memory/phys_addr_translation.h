#ifndef PHYS_ADDR_TRANSLATION_H
#define PHYS_ADDR_TRANSLATION_H
#include <ktypes.h>

EXTERN_C uint64_t physToVirtAddr(uint64_t paddr);
EXTERN_C uint64_t virtToPhysAddr(uint64_t vaddr);

#define __pa(vaddr) virtToPhysAddr(vaddr)
#define __va(paddr) physToVirtAddr(paddr)

#endif
