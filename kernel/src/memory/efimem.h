#ifndef EFIMEM_H
#define EFIMEM_H
#include <ktypes.h>

struct EFI_MEMORY_DESCRIPTOR {
    uint32_t    type;
    void*       paddr;
    void*       vaddr;
    uint64_t    pageCount;
    uint64_t    attribs;
};

extern const char* EFI_MEMORY_TYPE_STRINGS[];

#endif