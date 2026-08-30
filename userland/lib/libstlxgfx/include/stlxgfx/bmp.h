#ifndef STLXGFX_BMP_H
#define STLXGFX_BMP_H

#include <stlxgfx/surface.h>

#ifdef __cplusplus
extern "C" {
#endif

stlxgfx_surface_t* stlxgfx_load_bmp(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* STLXGFX_BMP_H */
