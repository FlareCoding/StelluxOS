#include "kdisplay.h"
#include <kmemory.h>

struct Psf1Hdr {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charSize;
};

struct Psf1Font {
    Psf1Hdr* header;
    void* glyphBuffer;
};

Framebuffer Display::s_framebuffer;
void* Display::s_font = nullptr;

void Display::initialize(void* framebuffer, void* font) {
    memcpy(&s_framebuffer, framebuffer, sizeof(Framebuffer));

    s_font = font;
}

void Display::fillPixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t* base = (uint32_t*)s_framebuffer.base;
    uint32_t* pixelPtr = base + x + (y * s_framebuffer.pixelsPerScanline);

    *pixelPtr = color;
}
