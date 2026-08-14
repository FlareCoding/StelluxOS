#ifndef STLXGFX_IMAGE_H
#define STLXGFX_IMAGE_H

#include <stlxgfx/surface.h>

/* Load a BMP, PNG, or JPEG file into a 32-bit BGRA surface, picking
 * the decoder from the file's magic bytes. Returns NULL if the file
 * cannot be read or decoded. */
stlxgfx_surface_t* stlxgfx_load_image(const char* path);

#endif /* STLXGFX_IMAGE_H */
