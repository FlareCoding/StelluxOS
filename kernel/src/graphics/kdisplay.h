#ifndef KDISPLAY_H
#define KDISPLAY_H
#include <ktypes.h>

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

class Display {
public:
    static void initialize(
        void* framebuffer,
        void* font
    );

    static void fillPixel(uint32_t x, uint32_t y, uint32_t color);
    static void renderTextGlyph(char chr, uint32_t& x, uint32_t& y, uint32_t color);

    static inline Framebuffer& getFramebuffer() { return s_framebuffer; }
    static inline Psf1Font* getTextFontInfo() { return s_font; }

private:
    static Framebuffer s_framebuffer;
    static Psf1Font*   s_font;
};

#endif
