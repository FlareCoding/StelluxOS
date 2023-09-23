#ifndef KERNEL_PARAMS_H
#define KERNEL_PARAMS_H
#include "ktypes.h"

struct KernelEntryParams {
    struct {
        void*     base;
        uint64_t  size;
        uint32_t  width;
        uint32_t  height;
        uint32_t  pixelsPerScanline;
    } graphicsFramebuffer;

    struct {
        void*     base;
        uint64_t  size;
        uint64_t  descriptorSize;
        uint64_t  descriptorCount;
    } efiMemoryMap;
};

#endif // KERNEL_PARAMS_H