#include "vga_driver.h"
#include <memory/kmemory.h>
#include <interrupts/interrupts.h>
#include <paging/phys_addr_translation.h>
#include <paging/tlb.h>

#define CHAR_PIXEL_WIDTH 8

__PRIVILEGED_DATA
Framebuffer VGADriver::s_framebuffer;

__PRIVILEGED_DATA
Psf1Font* VGADriver::s_font = nullptr;

__PRIVILEGED_DATA
uint32_t* s_backBuffer;

__PRIVILEGED_CODE
void VGADriver::initialize(void* framebuffer, void* font) {
    // Copy framebuffer info
    memcpy(&s_framebuffer, framebuffer, sizeof(Framebuffer));

    // Set the font pointer
    s_font = static_cast<Psf1Font*>(font);
    s_font->header = static_cast<Psf1Hdr*>(__va(s_font->header));
    s_font->glyphBuffer = __va(s_font->glyphBuffer);

    // Clear the framebuffer
    memset(s_framebuffer.base, 0xA, s_framebuffer.size);

    // Update dimensions of the back buffer
    s_backBuffer = (uint32_t*)kmalloc(s_framebuffer.size);

    // Clear the back buffer
    memset(s_backBuffer, 0xA, s_framebuffer.size);

    // Set the framebuffer pages to use Write-Combining access type
    uint64_t gopBase = (uint64_t)s_framebuffer.base;
    uint64_t gopSize = s_framebuffer.size;
    for (
        uint64_t page = gopBase;
        page < gopBase + gopSize;
        page += PAGE_SIZE
    ) {
        paging::pte_t* pte = paging::getPteForAddr((void*)page, paging::g_kernelRootPageTable);
        pte->pageAccessType = 1;
    }

    paging::flushTlbAll();
}

__PRIVILEGED_CODE
void VGADriver::fillPixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t* base = static_cast<uint32_t*>(s_backBuffer);
    uint32_t* pixelPtr = base + x + (y * s_framebuffer.width);

    *pixelPtr = color;
}

__PRIVILEGED_CODE
void VGADriver::renderTextGlyph(char chr, uint32_t& x, uint32_t& y, uint32_t color) {
    uint8_t charPixelHeight = s_font->header->charSize;

    char* fontBuffer = static_cast<char*>(s_font->glyphBuffer) + (chr * charPixelHeight);

    // Detect overflow and implement text buffer scrolling
    if (y + charPixelHeight > s_framebuffer.height) {
        // Calculate the size of a full line in bytes
        const unsigned long fullLineWidthInBytes = s_framebuffer.width * sizeof(uint32_t); 

        // Pointers to the source and destination lines for copying
        uint8_t* dstLine = reinterpret_cast<uint8_t*>(s_backBuffer);
        uint8_t* srcLine = dstLine + (charPixelHeight * fullLineWidthInBytes);

        // Shift each line up by 'charPixelHeight' lines
        for (unsigned long offy = charPixelHeight; offy < s_framebuffer.height; ++offy) {
            memcpy(dstLine, srcLine, fullLineWidthInBytes);
            dstLine += fullLineWidthInBytes;
            srcLine += fullLineWidthInBytes;
        }

        // Clear the last line
        for (unsigned long offy = s_framebuffer.height - charPixelHeight; offy < s_framebuffer.height; ++offy) {
            for (unsigned long offx = 0; offx < s_framebuffer.width; ++offx) {
                fillPixel(offx, offy, 0xA0A0A0A);
            }
        }

        // Update the y-coordinate for the next character to be printed
        y -= charPixelHeight;  
    }

    // First we clear the character slot in case something is already there
    for (unsigned long yOffset = y; yOffset < y + charPixelHeight; yOffset++) {
		for (unsigned long xOffset = x; xOffset < x + CHAR_PIXEL_WIDTH; xOffset++) {
			fillPixel(xOffset, yOffset, 0xA0A0A0A);
		}
	}

	// Stellux kernel currently only supports the psf1 font that's 8xCHAR_PIXEL_HEIGHT px.
	for (unsigned long yOffset = y; yOffset < y + charPixelHeight; yOffset++) {
		for (unsigned long xOffset = x; xOffset < x + CHAR_PIXEL_WIDTH; xOffset++) {
			if ((*fontBuffer & (0b10000000 >> (xOffset - x))) > 0) {
				fillPixel(xOffset, yOffset, color);
			}
		}

		++fontBuffer;
	}
}

void VGADriver::swapBuffers() {
    // Interrupts have to be disabled between writing
    // to an I/O device to avoid race conditions.
    bool initialIrqFlag = areInterruptsEnabled();
    if (initialIrqFlag) {
        disableInterrupts();
    }
    
    for (size_t i = 0; i < s_framebuffer.height; ++i) {
        char* dest = (char*)s_framebuffer.base + (i * s_framebuffer.pixelsPerScanline * sizeof(uint32_t));
        char* src = (char*)s_backBuffer + (i * s_framebuffer.width * sizeof(uint32_t));
        memcpy(dest, src, s_framebuffer.width * sizeof(uint32_t));
    }

    // Re-enable interrupts if we entered this code path with IF=1
    if (initialIrqFlag) {
        enableInterrupts();
    }
}


void VGADriver::drawRectangle(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    // Validate coordinates and size
    if (x + width > s_framebuffer.width || y + height > s_framebuffer.height) {
        return;
    }

    uint32_t* pixelBase = (uint32_t*)s_framebuffer.base;
    uint32_t rowSize = width * sizeof(uint32_t); // size in bytes of the rectangle width

    // Temporary array to hold color values for a single memcpy operation
    uint32_t tempRow[width];
    for (uint32_t i = 0; i < width; ++i) {
        tempRow[i] = color;
    }

    for (uint32_t row = y; row < y + height; ++row) {
        uint32_t* rowStart = pixelBase + row * s_framebuffer.pixelsPerScanline + x;
        memcpy(rowStart, tempRow, rowSize); // Copy the entire row at once
    }
}