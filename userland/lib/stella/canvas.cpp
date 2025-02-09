#include "canvas.h"

#define abs(a) (((a) < 0) ? -(a) : (a))

namespace stella_ui {

canvas::canvas(const modules::gfx_framebuffer_module::framebuffer_t& fb, psf1_font* font)
    : m_framebuffer(fb), m_font(font), m_backgorund_color(0xff222222) {}

void canvas::clear() {
    // Depending on bpp, you can optimize. For example, if bpp == 8, we do a single memset.
    uint32_t fb_size = m_framebuffer.pitch * m_framebuffer.height;
    switch (m_framebuffer.bpp) {
    case 8: {
        // Truncate the 32-bit color to 8 bits
        memset(m_framebuffer.data, (uint8_t)(m_backgorund_color & 0xFF), fb_size);
        break;
    }
    case 16: {
        // Each pixel is 2 bytes
        uint16_t c16 = (uint16_t)(m_backgorund_color & 0xFFFF);
        for (uint32_t i = 0; i < m_framebuffer.width * m_framebuffer.height; i++) {
            ((uint16_t*)m_framebuffer.data)[i] = c16;
        }
        break;
    }
    case 24: {
        // We'll fill row by row
        // color_t is 0xAARRGGBB, but we only want R, G, B
        uint8_t r = (m_backgorund_color >> 16) & 0xFF;
        uint8_t g = (m_backgorund_color >>  8) & 0xFF;
        uint8_t b = (m_backgorund_color      ) & 0xFF;

        for (uint32_t y = 0; y < m_framebuffer.height; y++) {
            uint8_t* row = m_framebuffer.data + y * m_framebuffer.pitch;
            for (uint32_t x = 0; x < m_framebuffer.width; x++) {
                uint32_t index = x * 3;
                row[index + 0] = b;
                row[index + 1] = g;
                row[index + 2] = r;
            }
        }
        break;
    }
    case 32: {
        // Each pixel is 4 bytes
        uint32_t* ptr = (uint32_t*)m_framebuffer.data;
        uint32_t count = m_framebuffer.width * m_framebuffer.height;
        for (uint32_t i = 0; i < count; i++) {
            ptr[i] = m_backgorund_color;
        }
        break;
    }
    default:
        // Fallback: just do pixel by pixel if an unknown bpp
        for (uint32_t y = 0; y < m_framebuffer.height; y++) {
            for (uint32_t x = 0; x < m_framebuffer.width; x++) {
                draw_pixel(x, y, m_backgorund_color);
            }
        }
        break;
    }
}

void canvas::draw_pixel(int x, int y, color_t color) {
    if (x < 0 || y < 0) return;
    if ((uint32_t)x >= m_framebuffer.width ||
        (uint32_t)y >= m_framebuffer.height) {
        return;
    }

    switch (m_framebuffer.bpp) {
    case 8:
        put_pixel_8bpp(x, y, color);
        break;
    case 16:
        put_pixel_16bpp(x, y, color);
        break;
    case 24:
        put_pixel_24bpp(x, y, color);
        break;
    case 32:
        put_pixel_32bpp(x, y, color);
        break;
    default:
        // Fallback
        put_pixel_8bpp(x, y, color);
        break;
    }
}

void canvas::put_pixel_8bpp(int x, int y, color_t color) {
    uint32_t offset = y * m_framebuffer.pitch + x;
    m_framebuffer.data[offset] = (uint8_t)(color & 0xFF);
}

void canvas::put_pixel_16bpp(int x, int y, color_t color) {
    uint32_t offset = y * m_framebuffer.pitch + x * 2;
    uint16_t c16 = (uint16_t)(color & 0xFFFF);
    *(uint16_t*)&m_framebuffer.data[offset] = c16;
}

void canvas::put_pixel_24bpp(int x, int y, color_t color) {
    // color is 0xAARRGGBB
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >>  8) & 0xFF;
    uint8_t b = (color      ) & 0xFF;

    uint32_t offset = y * m_framebuffer.pitch + (x * 3);
    m_framebuffer.data[offset + 0] = b;
    m_framebuffer.data[offset + 1] = g;
    m_framebuffer.data[offset + 2] = r;
}

void canvas::put_pixel_32bpp(int x, int y, color_t color) {
    uint32_t offset = y * m_framebuffer.pitch + (x * 4);
    *(uint32_t*)&m_framebuffer.data[offset] = color;
}

void canvas::draw_line(int x0, int y0, int x1, int y1, color_t color) {
    // We can just forward to a helper
    draw_line_bresenham(x0, y0, x1, y1, color);
}

void canvas::draw_line_bresenham(int x0, int y0, int x1, int y1, color_t color) {
    // Bresenham’s line
    int dx = abs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy; // same as err = dx - dy, but dy is negative

    while (true) {
        draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void canvas::fill_rect(int x, int y, int w, int h, color_t color) {
    // Clip or skip if out of range
    if (w <= 0 || h <= 0) return;
    if (x >= (int)width() || y >= (int)height()) return;
    if (x + w < 0 || y + h < 0) return;

    int x_end = x + w;
    int y_end = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x_end > (int)width())  x_end = (int)width();
    if (y_end > (int)height()) y_end = (int)height();

    // For each scan line, do a horizontal fill
    for (int row = y; row < y_end; row++) {
        for (int col = x; col < x_end; col++) {
            draw_pixel(col, row, color);
        }
    }
}


void canvas::draw_rect(int x, int y, int w, int h, color_t color) {
    // We can just draw the 4 edges via draw_line
    int x2 = x + w - 1;
    int y2 = y + h - 1;

    // If w or h are 1 or 2, handle gracefully
    if (w <= 0 || h <= 0) return;
    // top edge
    draw_line(x, y, x2, y, color);
    // bottom edge
    draw_line(x, y2, x2, y2, color);
    // left edge
    draw_line(x, y, x, y2, color);
    // right edge
    draw_line(x2, y, x2, y2, color);
}

void canvas::draw_char(int x, int y, char c, color_t color) {
    if (!m_font) {
        return;
    }

    // Convert char c to an index (e.g., 'A' => 65, etc.)
    unsigned char uc = static_cast<unsigned char>(c);
    // Ensure we don't go out of range
    uint32_t glyph_index = uc % m_font->glyph_count;

    const uint8_t* glyph_ptr = m_font->glyph_data 
                             + (glyph_index * m_font->height);

    for (int row = 0; row < m_font->header.char_height; row++) {
        uint8_t row_bits = glyph_ptr[row];

        // For each of the 8 bits in row_bits, 0 => skip, 1 => draw
        for (int col = 0; col < 8; col++) {
            // Check if bit (7 - col) is set
            if (row_bits & (0x80 >> col)) {
                // Draw the pixel
                draw_pixel(x + col, y + row, color);
            }
        }
    }
}

void canvas::draw_string(int x, int y, const char* str, color_t color) {
    if (!m_font) {
        return;
    }

    int cursor_x = x;
    int cursor_y = y;

    // Each character is 8 wide, and "header.char_height" tall
    int glyph_width  = m_font->width;
    int glyph_height = m_font->height;

    while (*str) {
        char c = *str++;
        if (c == '\n') {
            // Move down one line, reset X to start
            cursor_x = x;
            cursor_y += glyph_height;
        } else {
            // Draw one character
            draw_char(cursor_x, cursor_y, c, color);
            cursor_x += glyph_width;
        }
    }
}

void canvas::composite_canvas(int dst_x, int dst_y, canvas* src_canvas) {
    // Ensure the source is within bounds of the destination canvas
    if (dst_x >= (int)width() || dst_y >= (int)height()) {
        return; // Completely out of bounds
    }

    auto& src_framebuffer = src_canvas->m_framebuffer;

    // Determine the effective copy region (clipping)
    int src_width = src_framebuffer.width;
    int src_height = src_framebuffer.height;
    int copy_width = kstl::min(src_width, (int)width() - dst_x);
    int copy_height = kstl::min(src_height, (int)height() - dst_y);

    // Adjust for negative dst_x or dst_y (partial out-of-bounds cases)
    int x_offset = (dst_x < 0) ? -dst_x : 0;
    int y_offset = (dst_y < 0) ? -dst_y : 0;

    dst_x = kstl::max(0, dst_x);
    dst_y = kstl::max(0, dst_y);

    // Optimization path: if bpp matches, perform fast row-by-row memcpy
    if (m_framebuffer.bpp == src_framebuffer.bpp) {
        for (int y = y_offset; y < copy_height; ++y) {
            uint8_t* src_row = src_framebuffer.data + y * src_framebuffer.pitch;
            uint8_t* dst_row = m_framebuffer.data + (dst_y + y) * m_framebuffer.pitch + dst_x * (m_framebuffer.bpp / 8);

            memcpy(dst_row, src_row + x_offset * (src_framebuffer.bpp / 8), copy_width * (src_framebuffer.bpp / 8));
        }
    } else {
        // Fallback path: copy pixel by pixel and convert between formats if needed
        for (int y = y_offset; y < copy_height; ++y) {
            for (int x = x_offset; x < copy_width; ++x) {
                // Calculate pixel position in the source framebuffer
                int src_offset = y * src_framebuffer.pitch + x * (src_framebuffer.bpp / 8);
                
                color_t src_color = 0;
                switch (src_framebuffer.bpp) {
                    case 8:
                        src_color = src_framebuffer.data[src_offset];
                        break;
                    case 16:
                        src_color = *(uint16_t*)(src_framebuffer.data + src_offset);
                        break;
                    case 24:
                        src_color = src_framebuffer.data[src_offset] |
                                    (src_framebuffer.data[src_offset + 1] << 8) |
                                    (src_framebuffer.data[src_offset + 2] << 16);
                        break;
                    case 32:
                        src_color = *(uint32_t*)(src_framebuffer.data + src_offset);
                        break;
                }

                // Draw the pixel on the destination canvas
                draw_pixel(dst_x + x, dst_y + y, src_color);
            }
        }
    }
}

} // namespace stella_ui

