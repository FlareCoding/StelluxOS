#ifndef STLXDM_DECOR_HPP
#define STLXDM_DECOR_HPP

#include "damage.hpp"

#include <stlxgfx/surface.h>

#include <cstdint>

struct dm_window;

/* Server-side window chrome. Window x and y remain the content origin
 * everywhere, decorations extend around it, and borderless windows
 * have none. */
namespace decor {

constexpr int32_t TITLE_H = 28;
constexpr int32_t BORDER = 2;
constexpr int32_t CLOSE_R = 8;
constexpr int32_t CLOSE_MARGIN = 14;

/* Pointer zones inside a window's decorated bounds */
enum class zone {
    none,
    content,
    title,
    close,
};

bool decorated(const dm_window& w);

/* The full on-screen rect including chrome, the content rect when
 * borderless. Zero when the window has no displayed buffer. */
damage_list::rect bounds(const dm_window& w);

/* Zone under a screen point, none when outside the window. */
zone hit(const dm_window& w, int32_t x, int32_t y);

/* Draws the chrome around the content area, clipped by the caller's
 * compose rect. */
void draw(stlxgfx_surface_t* back, const dm_window& w, bool focused,
          bool close_hover);

} // namespace decor

#endif
