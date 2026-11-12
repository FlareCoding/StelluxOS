#ifndef STLXDM_CAPTURE_GLYPHS_HPP
#define STLXDM_CAPTURE_GLYPHS_HPP

#include <stlxgfx/surface.h>

#include <cstdint>

/* The marks a capture's surfaces draw: the capture bar's modes, a card's actions, and the warning of a card without
 * memory */
enum class capture_glyph { region, window, screen, preview, discard, close, warning };

/* `glyph` filled `size` pixels square in `color`, which the caller destroys, or null without memory */
stlxgfx_surface_t* make_capture_glyph(capture_glyph glyph, int32_t size, uint32_t color);

#endif
