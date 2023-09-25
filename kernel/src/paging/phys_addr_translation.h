#ifndef PHYS_ADDR_TRANSLATION_H
#define PHYS_ADDR_TRANSLATION_H
#include <ktypes.h>

EXTERN_C void* physToVirtAddr(void* paddr);
EXTERN_C void* virtToPhysAddr(void* vaddr);

#define __pa(vaddr) virtToPhysAddr(vaddr)
#define __va(paddr) physToVirtAddr(paddr)

#endif
