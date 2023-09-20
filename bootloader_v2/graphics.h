#ifndef GRAPHICS_H
#define GRAPHICS_H
#include <efi.h>
#include <efilib.h>

typedef struct Framebuffer {
    void*       Base;
    uint64_t    Size;
    uint32_t    Width;
    uint32_t    Height;
    uint32_t    PixelsPerScanline;
} Framebuffer_t;

Framebuffer_t*
InitGraphicsProtocol(
    EFI_SYSTEM_TABLE* SystemTable
);

#endif
