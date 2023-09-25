#include "kdisplay.h"
#include <memory/kmemory.h>

#define CHAR_PIXEL_WIDTH 8

Framebuffer Display::s_framebuffer;
Psf1Font* Display::s_font = nullptr;

void Display::initialize(void* framebuffer, void* font) {
    // Copy framebuffer info
    memcpy(&s_framebuffer, framebuffer, sizeof(Framebuffer));

    // Set the font pointer
    s_font = static_cast<Psf1Font*>(font);

    // Clear the framebuffer
    zeromem(s_framebuffer.base, s_framebuffer.size);
}

void Display::fillPixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t* base = static_cast<uint32_t*>(s_framebuffer.base);
    uint32_t* pixelPtr = base + x + (y * s_framebuffer.pixelsPerScanline);

    *pixelPtr = color;
}

void Display::renderTextGlyph(char chr, uint32_t& x, uint32_t& y, uint32_t color) {
    uint8_t charPixelHeight = s_font->header->charSize;

    char* fontBuffer = static_cast<char*>(s_font->glyphBuffer) + (chr * charPixelHeight);

    // Detect overflow and implement text buffer scrolling
    if (y + charPixelHeight > s_framebuffer.height) {
        for (unsigned long offy = charPixelHeight; offy < s_framebuffer.height + charPixelHeight; offy++) {
            for (unsigned long offx = 0; offx < s_framebuffer.width; offx++) {
                // Get the color of the current filled pixel
                uint32_t* crntPx = static_cast<uint32_t*>(s_framebuffer.base) + offx + (offy * s_framebuffer.pixelsPerScanline);

                // Fill the previous line with the current
                // line's color creating a "copy-up" effect.
                fillPixel(offx, offy - charPixelHeight, *crntPx);
            }
        }

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
