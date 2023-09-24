#ifndef ELF_SEGMENT_INFO_H
#define ELF_SEGMENT_INFO_H
#include <ktypes.h>

#define MAX_LOADED_ELF_SEGMENTS 0x1C520

struct ElfSegmentInfo {
    void*       physicalBase;
    uint64_t    physicalSize;
    void*       virtualBase;
    uint64_t    virtualSize;
    uint32_t    flags;
};

#endif
