#include "entry_params.h"
#include <kmemory.h>
#include <graphics/kdisplay.h>
#include <gdt/gdt.h>
#include <kprint.h>

extern "C" void _kentry(KernelEntryParams* params);

void _kentry(KernelEntryParams* params) {
    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    intializeAndInstallGDT();

    // Initialize display and graphics context
    Display::initialize(&params->graphicsFramebuffer, params->textRenderingFont);

    kprintInfo("This is an info message: '%s'\n", "Hello!");
    kprintWarn("This is a warning (warning:%i)\n", 34);
    kprintError("This is an error [errcode:0x%x]\n", 0x02);
    kprint("This is a normal print statement\n");

    while (1) {
        __asm__ volatile("hlt");
    }
}
