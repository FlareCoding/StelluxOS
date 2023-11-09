#include "kdisplay.h"
#include <memory/kmemory.h>
#include <interrupts/interrupts.h>
#include <paging/phys_addr_translation.h>

#define CHAR_PIXEL_WIDTH 8

#define BACK_BUFFER_WIDTH   800
#define BACK_BUFFER_HEIGHT  600

__PRIVILEGED_DATA
Framebuffer Display::s_framebuffer;

__PRIVILEGED_DATA
Psf1Font* Display::s_font = nullptr;

__PRIVILEGED_DATA
uint32_t s_backBuffer[0x1D5000]; // 800x600 resolution

__PRIVILEGED_CODE
void Display::initialize(void* framebuffer, void* font) {
    // Copy framebuffer info
    memcpy(&s_framebuffer, framebuffer, sizeof(Framebuffer));

    // Set the font pointer
    s_font = static_cast<Psf1Font*>(font);
    s_font->header = static_cast<Psf1Hdr*>(__va(s_font->header));
    s_font->glyphBuffer = __va(s_font->glyphBuffer);

    // Clear the framebuffer
    memset(s_framebuffer.base, 0xA, s_framebuffer.size);

    // Clear the back buffer
    zeromem(s_backBuffer, BACK_BUFFER_WIDTH * BACK_BUFFER_HEIGHT * sizeof(uint32_t));
}

__PRIVILEGED_CODE
void Display::fillPixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t* base = static_cast<uint32_t*>(s_backBuffer);
    uint32_t* pixelPtr = base + x + (y * BACK_BUFFER_WIDTH);

    *pixelPtr = color;
}

__PRIVILEGED_CODE
void Display::renderTextGlyph(char chr, uint32_t& x, uint32_t& y, uint32_t color) {
    uint8_t charPixelHeight = s_font->header->charSize;

    char* fontBuffer = static_cast<char*>(s_font->glyphBuffer) + (chr * charPixelHeight);

    // Detect overflow and implement text buffer scrolling
    if (y + charPixelHeight > BACK_BUFFER_HEIGHT) {
        // Calculate the size of a full line in bytes
        const unsigned long fullLineWidthInBytes = BACK_BUFFER_WIDTH * sizeof(uint32_t); 

        // Pointers to the source and destination lines for copying
        uint8_t* dstLine = reinterpret_cast<uint8_t*>(s_backBuffer);
        uint8_t* srcLine = dstLine + (charPixelHeight * fullLineWidthInBytes);

        // Shift each line up by 'charPixelHeight' lines
        for (unsigned long offy = charPixelHeight; offy < BACK_BUFFER_HEIGHT; ++offy) {
            memcpy(dstLine, srcLine, fullLineWidthInBytes);
            dstLine += fullLineWidthInBytes;
            srcLine += fullLineWidthInBytes;
        }

        // Clear the last line
        for (unsigned long offy = BACK_BUFFER_HEIGHT - charPixelHeight; offy < BACK_BUFFER_HEIGHT; ++offy) {
            for (unsigned long offx = 0; offx < s_framebuffer.width; ++offx) {
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
}

void Display::swapBuffers() {
    // Interrupts have to be disabled between writing
    // to an I/O device to avoid race conditions.
    bool initialIrqFlag = areInterruptsEnabled();
    if (initialIrqFlag) {
        disableInterrupts();
    }
    
    for (size_t i = 0; i < BACK_BUFFER_HEIGHT; ++i) {
        char* dest = (char*)s_framebuffer.base + (i * s_framebuffer.width * sizeof(uint32_t));
        char* src = (char*)s_backBuffer + (i * BACK_BUFFER_WIDTH * sizeof(uint32_t));
        memcpy(dest, src, BACK_BUFFER_WIDTH * sizeof(uint32_t));
    }

    // Re-enable interrupts if we entered this code path with IF=1
    if (initialIrqFlag) {
        enableInterrupts();
    }
}
