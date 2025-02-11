#ifndef CANVAS_H
#define CANVAS_H
#include <modules/graphics/gfx_framebuffer_module.h>
#include <modules/module_manager.h>
#include <dynpriv/dynpriv.h>
#include <time/time.h>
#include <serial/serial.h>
#include "font.h"
#include "color.h"

namespace stella_ui {

using color_t       = uint32_t;
using framebuffer_t = modules::gfx_framebuffer_module::framebuffer_t;

class canvas {
public:
    canvas(const framebuffer_t& fb, psf1_font* font);

    void clear();

    void draw_pixel(int x, int y, color_t color);
    void draw_line(int x0, int y0, int x1, int y1, color_t color);

    void fill_rect(int x, int y, int w, int h, color_t color);
    void draw_rect(int x, int y, int w, int h, color_t color);

    void draw_char(int x, int y, char c, color_t color);
    void draw_string(int x, int y, const char* str, color_t color);

    void composite_canvas(int dst_x, int dst_y, canvas* src_canvas);

    inline uint32_t width() const  { return m_framebuffer.width; }
    inline uint32_t height() const { return m_framebuffer.height; }

    inline color_t get_background_color() const { return m_backgorund_color; }
    inline void set_background_color(color_t color) { m_backgorund_color = color; }

    inline framebuffer_t& get_native_framebuffer() { return m_framebuffer; }

private:
    framebuffer_t   m_framebuffer;
    psf1_font*      m_font;
    color_t         m_backgorund_color;

    void put_pixel_8bpp(int x, int y, color_t color);
    void put_pixel_16bpp(int x, int y, color_t color);
    void put_pixel_24bpp(int x, int y, color_t color);
    void put_pixel_32bpp(int x, int y, color_t color);

    void draw_line_bresenham(int x0, int y0, int x1, int y1, color_t color);
};

} // namespace stella_ui

#endif // CANVAS_H

