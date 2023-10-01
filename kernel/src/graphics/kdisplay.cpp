#include "kdisplay.h"
#include <memory/kmemory.h>
#include <interrupts/interrupts.h>
#include <paging/phys_addr_translation.h>

#define CHAR_PIXEL_WIDTH 8

Framebuffer Display::s_framebuffer;
Psf1Font* Display::s_font = nullptr;

void Display::initialize(void* framebuffer, void* font) {
    // Copy framebuffer info
    memcpy(&s_framebuffer, framebuffer, sizeof(Framebuffer));

    // Set the font pointer
    s_font = static_cast<Psf1Font*>(font);
    s_font->header = static_cast<Psf1Hdr*>(__va(s_font->header));
    s_font->glyphBuffer = __va(s_font->glyphBuffer);

    // Clear the framebuffer
    zeromem(s_framebuffer.base, s_framebuffer.size);
}

void Display::fillPixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t* base = static_cast<uint32_t*>(s_framebuffer.base);
    uint32_t* pixelPtr = base + x + (y * s_framebuffer.pixelsPerScanline);

    *pixelPtr = color;
}

void Display::renderTextGlyph(char chr, uint32_t& x, uint32_t& y, uint32_t color) {
    // Interrupts have to be disabled between writing
    // to an I/O device to avoid race conditions.
    bool initialIrqFlag = areInterruptsEnabled();
    if (initialIrqFlag) {
        disableInterrupts();
    }

    uint8_t charPixelHeight = s_font->header->charSize;

    char* fontBuffer = static_cast<char*>(s_font->glyphBuffer) + (chr * charPixelHeight);

    // Detect overflow and implement text buffer scrolling
    if (y + charPixelHeight > s_framebuffer.height) {
        // Calculate the size of a full line in bytes
        const unsigned long fullLineWidth = s_framebuffer.pixelsPerScanline * sizeof(uint32_t); 

        // Pointers to the source and destination lines for copying
        uint8_t* dstLine = static_cast<uint8_t*>(s_framebuffer.base);
        uint8_t* srcLine = dstLine + (charPixelHeight * fullLineWidth);

        // Shift each line up by 'charPixelHeight' lines
        for (unsigned long offy = charPixelHeight; offy < s_framebuffer.height; ++offy) {
            memcpy(dstLine, srcLine, fullLineWidth);
            dstLine += fullLineWidth;
            srcLine += fullLineWidth;
        }

        // Clear the last line
        for (unsigned long offy = s_framebuffer.height - charPixelHeight; offy < s_framebuffer.height; ++offy) {
            for (unsigned long offx = 0; offx < s_framebuffer.pixelsPerScanline; ++offx) {
                fillPixel(offx, offy, 0);  // Assuming 0 is the 'clear' color
            }
        }

        // Update the y-coordinate for the next character to be printed
        y -= charPixelHeight;  
    }

    // First we clear the character slot in case something is already there
    for (unsigned long yOffset = y; yOffset < y + charPixelHeight; yOffset++) {
		for (unsigned long xOffset = x; xOffset < x + CHAR_PIXEL_WIDTH; xOffset++) {
			fillPixel(xOffset, yOffset, 0);
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

    // Re-enable interrupts if we entered this code path with IF=1
    if (initialIrqFlag) {
        enableInterrupts();
    }
}
