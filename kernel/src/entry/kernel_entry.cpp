#include "entry_params.h"
#include "kmemory.h"
#include "gdt/gdt.h"

extern "C" void _kentry(KernelEntryParams* params);

void setPixel(int x, int y, int color, KernelEntryParams* params) {
    uint32_t* framebuffer = static_cast<uint32_t*>(params->graphicsFramebuffer.base);
    framebuffer[x + (y * params->graphicsFramebuffer.width)] = color;
}

void _kentry(KernelEntryParams* params) {
    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    intializeAndInstallGDT();

    // Draw a colored square in the top left corner
    for (int x = 0; x < 100; ++x) {
        for (int y = 0; y < 100; ++y) {
            setPixel(x, y, 0xFFEFFFFF, params);
        }
    }

    while (1) {
        __asm__ volatile("hlt");
    }
}
