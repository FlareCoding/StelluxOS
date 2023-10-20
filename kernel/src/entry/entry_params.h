#ifndef KERNEL_PARAMS_H
#define KERNEL_PARAMS_H
#include "elf_segment_info.h"

struct KernelEntryParams {
    ElfSegmentInfo* kernelElfSegments;

    struct {
        void*     base;
        uint64_t  size;
        uint32_t  width;
        uint32_t  height;
        uint32_t  pixelsPerScanline;

    } graphicsFramebuffer;

    void* textRenderingFont;

    struct {
        void*     base;
        uint64_t  size;
        uint64_t  descriptorSize;
        uint64_t  descriptorCount;
    } efiMemoryMap;

    void* kernelStack;
    void* rsdp;
};

#endif // KERNEL_PARAMS_H