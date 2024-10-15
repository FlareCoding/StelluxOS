#ifndef VGA_DRIVER_H
#define VGA_DRIVER_H
#include <entry/entry_params.h>

struct Point {
    uint32_t x;
    uint32_t y;
};

struct Psf1Hdr {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charSize;
};

struct Psf1Font {
    Psf1Hdr* header;
    void* glyphBuffer;
};

struct Framebuffer {
	void*       base;
	uint64_t    size;
	uint32_t    width;
	uint32_t    height;
	uint32_t    pixelsPerScanline;
};

class VGADriver {
public:
    __PRIVILEGED_CODE
    static void initialize(
        void* framebuffer,
        void* font
    );

    __PRIVILEGED_CODE
    static void fillPixel(uint32_t x, uint32_t y, uint32_t color);
    
    __PRIVILEGED_CODE
    static void renderTextGlyph(char chr, uint32_t& x, uint32_t& y, uint32_t color);

    static Framebuffer& getFramebuffer() { return s_framebuffer; }
    static inline Psf1Font* getTextFontInfo() { return s_font; }

    __PRIVILEGED_CODE
    static void swapBuffers();

    __PRIVILEGED_CODE
    static void drawRectangle(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);

private:
    __PRIVILEGED_DATA
    static Framebuffer s_framebuffer;

    __PRIVILEGED_DATA
    static Psf1Font*   s_font;
};

#endif

