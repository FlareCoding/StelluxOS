#include "window.h"
#include <memory/paging.h>

namespace stella_ui {
bool window_base::setup() {
    // Compute the real window size including borders and the title bar
    real_window_size.width = window_size.width + 2 * window_border_thickness;
    real_window_size.height = window_size.height + 2 * window_border_thickness + title_bar_height;

    framebuffer_t fb;
    fb.width = window_size.width;
    fb.height = window_size.height;
    fb.bpp = 32;
    fb.pitch = window_size.width * (fb.bpp / 8);
    fb.data = reinterpret_cast<uint8_t*>(zmalloc(fb.pitch * fb.height));

    if (!fb.data) {
        serial::printf("[!] Failed to create a window\n");
        return false;
    }

    psf1_font* font = _load_system_font();
    if (!font) {
        return false;
    }

    m_canvas = kstl::make_shared<canvas>(fb, font);

    m_canvas->set_background_color(background_color.to_argb());

    return true;
}

void window_base::draw_decorations(kstl::shared_ptr<canvas>& cvs) {
    // Define colors for decorations
    color_t border_color = color(0, 0, 0).to_argb();
    color_t title_bar_color = color(98, 102, 84).to_argb();
    color_t close_button_color = color(43, 43, 42).to_argb();
    color_t text_color = color::white.to_argb();

    // Fill the title bar area
    cvs->fill_rect(
        position.x,
        position.y,
        real_window_size.width,
        title_bar_height,
        title_bar_color
    );

    // Draw the title bar border
    cvs->draw_rect(
        position.x,
        position.y,
        real_window_size.width,
        title_bar_height,
        border_color
    );

    // Draw the close button
    int close_button_size = title_bar_height - 8;
    int close_button_x = position.x + real_window_size.width - window_border_thickness - close_button_size - 4;
    int close_button_y = position.y + window_border_thickness + 4;

    cvs->fill_rect(
        close_button_x,
        close_button_y,
        close_button_size,
        close_button_size,
        close_button_color
    );

    // Draw the "X" in the center of the close button
    int x_text_offset = close_button_x + (close_button_size / 2) - 4;  // Adjust for character width
    int y_text_offset = close_button_y + (close_button_size / 2) - 8;  // Adjust for character height
    cvs->draw_char(x_text_offset, y_text_offset, 'X', text_color);

    // Draw the window title (centered vertically within the title bar)
    int title_x_offset = window_border_thickness + 8;  // Small padding from the left
    int title_y_offset = window_border_thickness + (title_bar_height / 2) - 8;  // Center the text vertically
    cvs->draw_string(position.x + title_x_offset, position.y + title_y_offset, title.c_str(), text_color);

    // Draw the outer window border
    cvs->draw_rect(position.x, position.y, real_window_size.width, real_window_size.height, border_color);
}

point window_base::get_canvas_position() const {
    return point{
        .x = position.x + static_cast<int32_t>(window_border_thickness),
        .y = position.y + static_cast<int32_t>(window_border_thickness + title_bar_height),
        .z = position.z
    };
}
} // namespace stella_ui

