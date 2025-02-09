#ifndef PSF1_H
#define PSF1_H
#include <types.h>

struct psf1_font_hdr {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t char_height;
};

struct psf1_font {
    psf1_font_hdr   header;
    const uint8_t*  glyph_data;
    size_t          glyph_count;
    uint32_t        width; 
    uint32_t        height;
};

#endif // PSF1_H

