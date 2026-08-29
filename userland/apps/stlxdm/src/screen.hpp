#ifndef STLXDM_SCREEN_HPP
#define STLXDM_SCREEN_HPP

#include <cstdint>

/* The scanout target: /dev/gfxfb mapped write-combining. Reads from
 * the mapping are pathologically slow, so all composition happens in
 * regular memory and only damaged rows are copied here. */
struct screen {
    int      fd = -1;
    uint8_t* scanout = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint16_t bpp = 0;
    uint8_t  red_shift = 0;
    uint8_t  green_shift = 0;
    uint8_t  blue_shift = 0;
    uint64_t size = 0;

    /* Opens and maps the framebuffer. Returns 0, or -1 without one. */
    int init();
    void shutdown();
};

#endif
