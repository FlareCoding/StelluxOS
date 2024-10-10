#include "vga_text_driver.h"
#include <memory/kmemory.h>
#include <kelevate/kelevate.h>
#include <paging/page.h>
#include <kstring.h>
#include <sync.h>

#define CHAR_PIXEL_WIDTH        8
#define CHAR_TOP_BORDER_OFFSET  8
#define CHAR_LEFT_BORDER_OFFSET 8

uint32_t VGATextDriver::s_width;
uint32_t VGATextDriver::s_height;
uint32_t VGATextDriver::s_pixelsPerScanline;

char* VGATextDriver::s_fontGlyphBuffer;
uint8_t VGATextDriver::s_fontCharSize;

uint32_t VGATextDriver::s_cursorPosX;
uint32_t VGATextDriver::s_cursorPosY;

DECLARE_SPINLOCK(s_vgaTextRenderingLock);

void VGATextDriver::init(uint32_t width, uint32_t height, uint32_t pixelsPerScanline, void* font) {
    auto psf1Font = reinterpret_cast<Psf1Font*>(font);

    s_width = width;
    s_height = height;
    s_pixelsPerScanline = pixelsPerScanline;

    const size_t fontPages = 2;
    s_fontGlyphBuffer = (char*)zallocPages(fontPages);

    RUN_ELEVATED({
        paging::mapPages(s_fontGlyphBuffer, psf1Font->glyphBuffer, fontPages, USERSPACE_PAGE, 0, paging::getCurrentTopLevelPageTable());
        s_fontCharSize = psf1Font->header->charSize;
    });
}

void VGATextDriver::setCursorPos(uint32_t x, uint32_t y) {
    // Validate coordinates
    if (x > s_width || y > s_height) {
        return;
    }

    s_cursorPosX = x;
    s_cursorPosY = y;
}

void VGATextDriver::renderChar(char chr, uint32_t color) {
    switch (chr) {
        case '\n': {
            s_cursorPosX = CHAR_LEFT_BORDER_OFFSET;
            s_cursorPosY += s_fontCharSize;

            //
            // I haven't figured out fully why whenever there was a double newline ('\n')
            // character printed, it caused the next line's first character to get erased,
            // so this line pre-writes an empty space with an absent color to fix this issue.
            //
            // *Note* a proper fix should be implemented later.
            //
            _renderChar(' ', NULL);
            break;
        }
        case '\r': {
            s_cursorPosX = CHAR_LEFT_BORDER_OFFSET;
            break;
        }
        default: {
            _renderChar(chr, color);
            s_cursorPosX += CHAR_PIXEL_WIDTH;

            if (s_cursorPosX + CHAR_PIXEL_WIDTH > s_width) {
                s_cursorPosX = CHAR_LEFT_BORDER_OFFSET;
                s_cursorPosY += s_fontCharSize;
            }
            break;
        }
    }
}

void VGATextDriver::renderString(const char* str, uint32_t color) {
    acquireSpinlock(&s_vgaTextRenderingLock);
    size_t len = strlen(str);
    for (size_t i = 0; i < len; i++) {
        renderChar(str[i], color);
    }
    releaseSpinlock(&s_vgaTextRenderingLock);
}

void VGATextDriver::_renderChar(char chr, uint32_t color) {
    char* fontBuffer = static_cast<char*>(s_fontGlyphBuffer) + (chr * s_fontCharSize);

    // Detect overflow and implement text buffer scrolling
    if (s_cursorPosY + s_fontCharSize > s_height) {
        // Calculate the size of a full line in bytes
        const unsigned long fullLineWidthInBytes = s_width * sizeof(uint32_t); 

        // Pointers to the source and destination lines for copying
        uint8_t* dstLine = reinterpret_cast<uint8_t*>(VGADriver::getDrawingContext());
        uint8_t* srcLine = dstLine + (s_fontCharSize * fullLineWidthInBytes);

        // Shift each line up by 'charPixelHeight' lines
        for (unsigned long offy = s_fontCharSize; offy < s_height; ++offy) {
            memcpy(dstLine, srcLine, fullLineWidthInBytes);
            dstLine += fullLineWidthInBytes;
            srcLine += fullLineWidthInBytes;
        }

        // Clear the last line
        for (unsigned long offy = s_height - s_fontCharSize; offy < s_height; ++offy) {
            for (unsigned long offx = 0; offx < s_width; ++offx) {
                VGADriver::renderPixel(offx, offy, 0xA0A0A0A);
            }
        }

        // Update the y-coordinate for the next character to be printed
        s_cursorPosY -= s_fontCharSize;  
    }

    // First we clear the character slot in case something is already there
    for (unsigned long yOffset = s_cursorPosY; yOffset < s_cursorPosY + s_fontCharSize; yOffset++) {
		for (unsigned long xOffset = s_cursorPosX; xOffset < s_cursorPosX + CHAR_PIXEL_WIDTH; xOffset++) {
			VGADriver::renderPixel(xOffset, yOffset, 0xA0A0A0A);
		}
	}

	// Stellux kernel currently only supports the psf1 font that's 8xCHAR_PIXEL_HEIGHT px.
	for (unsigned long yOffset = s_cursorPosY; yOffset < s_cursorPosY + s_fontCharSize; yOffset++) {
		for (unsigned long xOffset = s_cursorPosX; xOffset < s_cursorPosX + CHAR_PIXEL_WIDTH; xOffset++) {
			if ((*fontBuffer & (0b10000000 >> (xOffset - s_cursorPosX))) > 0) {
				VGADriver::renderPixel(xOffset, yOffset, color);
			}
		}

		++fontBuffer;
	}
}
