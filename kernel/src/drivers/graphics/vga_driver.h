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

struct VgaFramebuffer {
	void*       physicalBase;
	void*       virtualBase;
	uint64_t    size;
	uint32_t    width;
	uint32_t    height;
	uint32_t    pixelsPerScanline;
};

class VGADriver {
public:
    static void init(KernelEntryParams* params);

    static void renderPixel(uint32_t x, uint32_t y, uint32_t color);
    static void renderRectangle(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);

    static void swapBuffers();

    static uint32_t* getDrawingContext(); 

private:
    static VgaFramebuffer   s_vgaFramebuffer;
    static uint32_t*        s_backBuffer;
};

#endif

