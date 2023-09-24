#ifndef KDISPLAY_H
#define KDISPLAY_H
#include <ktypes.h>

struct Point {
    uint32_t x;
    uint32_t y;
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

    static inline Framebuffer& getFramebuffer() { return s_framebuffer; }

private:
    static Framebuffer s_framebuffer;
    static void*       s_font;
};

#endif
