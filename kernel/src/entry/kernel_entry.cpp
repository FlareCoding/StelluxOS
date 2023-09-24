#include "entry_params.h"
#include "kmemory.h"
#include "graphics/kdisplay.h"
#include "gdt/gdt.h"

extern "C" void _kentry(KernelEntryParams* params);

void _kentry(KernelEntryParams* params) {
    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    intializeAndInstallGDT();

    // Initialize display and graphics context
    Display::initialize(&params->graphicsFramebuffer, params->textRenderingFont);

    // Draw a colored square in the top left corner
    for (int x = 10; x < 60; ++x) {
        for (int y = 10; y < 60; ++y) {
            Display::fillPixel(x, y, 0x000FFFFF);
        }
    }

    while (1) {
        __asm__ volatile("hlt");
    }
}
