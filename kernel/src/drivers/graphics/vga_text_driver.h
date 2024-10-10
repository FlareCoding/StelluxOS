#ifndef VGA_TEXT_DRIVER_H
#define VGA_TEXT_DRIVER_H
#include "vga_driver.h"

class VGATextDriver {
public:
    static void init(uint32_t width, uint32_t height, uint32_t pixelsPerScanline, void* font);

    static void setCursorPos(uint32_t x, uint32_t y);

    static void renderChar(char chr, uint32_t color);
    static void renderString(const char* str, uint32_t color);

private:
    static uint32_t s_width;
    static uint32_t s_height;
    static uint32_t s_pixelsPerScanline;
    static Psf1Font* s_font;

    static uint32_t s_cursorPosX;
    static uint32_t s_cursorPosY;

    static void _renderChar(char chr, uint32_t color);
};

#endif
