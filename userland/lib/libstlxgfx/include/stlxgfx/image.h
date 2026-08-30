#ifndef STLXGFX_IMAGE_H
#define STLXGFX_IMAGE_H

#include <stlxgfx/surface.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load a BMP, PNG, or JPEG file into a 32-bit BGRA surface, picking
 * the decoder from the file's magic bytes. Returns NULL if the file
 * cannot be read or decoded. */
stlxgfx_surface_t* stlxgfx_load_image(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* STLXGFX_IMAGE_H */
