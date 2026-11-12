#ifndef STLXGFX_IMAGE_H
#define STLXGFX_IMAGE_H

#include <stlxgfx/surface.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load a BMP, PNG, or JPEG file into a 32-bit BGRA surface, picking
 * the decoder from the file's magic bytes. Returns NULL if the file
 * cannot be read or decoded. */
stlxgfx_surface_t* stlxgfx_load_image(const char* path);

/* Decode a PNG or JPEG held in memory into a 32-bit BGRA surface.
 * Returns NULL if the bytes cannot be decoded. */
stlxgfx_surface_t* stlxgfx_decode_image(const void* data, size_t len);

/* Encode a 32-bit surface as a PNG in memory the caller frees, `out_len` long. `keeps_alpha` writes alpha, taken out
 * of premultiplied pixels, and without it every pixel is opaque. NULL for another depth or without memory. */
unsigned char* stlxgfx_encode_png(const stlxgfx_surface_t* surface, int keeps_alpha, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* STLXGFX_IMAGE_H */
