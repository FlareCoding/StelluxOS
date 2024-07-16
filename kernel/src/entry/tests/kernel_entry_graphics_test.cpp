#include "kernel_entry_tests.h"
#include <core/kprint.h>
#include <graphics/kdisplay.h>
#include <kelevate/kelevate.h>
#include <time/ktime.h>

uint32_t rgbToUint32(uint8_t r, uint8_t g, uint8_t b) {
    // Assume the format is 0xAARRGGBB
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

void ke_test_graphics() {
    __kelevate();

    int colorVal = 0;

    while (true) {
        if (colorVal == 255) colorVal = 0;
        else colorVal = 255;

        Display::drawRectangle(100, 100, 600, 400, rgbToUint32(colorVal, 60, 50));
        sleep(1);
    }

    __klower();
}
