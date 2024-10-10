#include "vga_driver.h"
#include <memory/kmemory.h>
#include <paging/page.h>
#include <kelevate/kelevate.h>
#include <kprint.h>

VgaFramebuffer VGADriver::s_vgaFramebuffer;
uint32_t* VGADriver::s_backBuffer;

void VGADriver::init(KernelEntryParams* params) {
    s_vgaFramebuffer.physicalBase = params->graphicsFramebuffer.base;
    s_vgaFramebuffer.size = params->graphicsFramebuffer.size;
    s_vgaFramebuffer.width = params->graphicsFramebuffer.width;
    s_vgaFramebuffer.height = params->graphicsFramebuffer.height;
    s_vgaFramebuffer.pixelsPerScanline = params->graphicsFramebuffer.pixelsPerScanline;

    // Map the VGA memory to a usermode page
    size_t framebufferPages = s_vgaFramebuffer.size / PAGE_SIZE + 1;
    s_vgaFramebuffer.virtualBase = zallocPages(framebufferPages);
    
    RUN_ELEVATED({
        paging::mapPages(
            s_vgaFramebuffer.virtualBase,
            s_vgaFramebuffer.physicalBase,
            framebufferPages,
            USERSPACE_PAGE,
            PAGE_ATTRIB_ACCESS_TYPE,
            paging::getCurrentTopLevelPageTable()
        );
    });

    // Allocate a back buffer
    s_backBuffer = (uint32_t*)kzmalloc(s_vgaFramebuffer.size);

    // Perform initial clear screen
    renderRectangle(0, 0, s_vgaFramebuffer.width, s_vgaFramebuffer.height, 0x0A0A0A);
    swapBuffers();
}

void VGADriver::renderPixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t* pixelPtr = s_backBuffer + x + (y * s_vgaFramebuffer.width);
    *pixelPtr = color;
}

void VGADriver::renderRectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
) {
    // Validate coordinates and size
    if (x + width > s_vgaFramebuffer.width || y + height > s_vgaFramebuffer.height) {
        return;
    }

    uint32_t rowSize = width * sizeof(uint32_t); // size in bytes of the rectangle width

    // Temporary array to hold color values for a single memcpy operation
    uint32_t tempRow[width];
    for (uint32_t i = 0; i < width; ++i) {
        tempRow[i] = color;
    }

    for (uint32_t row = y; row < y + height; ++row) {
        uint32_t* rowStart = s_backBuffer + row * s_vgaFramebuffer.pixelsPerScanline + x;
        memcpy(rowStart, tempRow, rowSize); // Copy the entire row at once
    }
}

void VGADriver::swapBuffers() {
    // Interrupts have to be disabled between writing
    // to an I/O device to avoid race conditions.
    bool initialIrqFlag = areInterruptsEnabled();
    if (initialIrqFlag) {
        RUN_ELEVATED({disableInterrupts();});
    }
    
    for (size_t i = 0; i < s_vgaFramebuffer.height; ++i) {
        char* dest = (char*)s_vgaFramebuffer.virtualBase + (i * s_vgaFramebuffer.pixelsPerScanline * sizeof(uint32_t));
        char* src = (char*)s_backBuffer + (i * s_vgaFramebuffer.width * sizeof(uint32_t));
        memcpy(dest, src, s_vgaFramebuffer.width * sizeof(uint32_t));
    }

    // Re-enable interrupts if we entered this code path with IF=1
    if (initialIrqFlag) {
        RUN_ELEVATED({enableInterrupts();});
    }
}

uint32_t* VGADriver::getDrawingContext() {
    return s_backBuffer;
}

