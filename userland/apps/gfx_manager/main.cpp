#include "screen_manager.h"
#include <arch/x86/cpuid.h>

#include "sample_window_app.h"

extern uint64_t g_mouse_cursor_pos_x;
extern uint64_t g_mouse_cursor_pos_y;

void draw_cursor(kstl::shared_ptr<canvas>& cvs, int x, int y, uint32_t fill_color, uint32_t border_color) {
    static const char* cursor_shape[16] = {
        "X                 ",
        "XX                ",
        "X.X               ",
        "X..X              ",
        "X...X             ",
        "X....X            ",
        "X.....X           ",
        "X......X          ",
        "X.......X         ",
        "X........X        ",
        "X...XXXXXXX       ",
        "X..XX             ",
        "X.X               ",
        "XX                ",
        "X                 ",
        "                  "
    };

    const int height = 16;
    const int width = 16;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            if (cursor_shape[row][col] == 'X') {
                cvs->fill_rect(x + col, y + row, 1, 1, border_color);
            } else if (cursor_shape[row][col] == '.') {
                cvs->fill_rect(x + col, y + row, 1, 1, fill_color);
            }
        }
    }
}

void draw_close_button_icon(kstl::shared_ptr<canvas>& cvs, int x, int y, int size, color_t color) {
    // Draw an "X" icon inside the close button
    int padding = size / 4;  // Slight padding from edges of the button
    int x0 = x + padding;
    int y0 = y + padding;
    int x1 = x + size - padding;
    int y1 = y + size - padding;

    // Two diagonal lines forming an "X"
    cvs->draw_line(x0, y0, x1, y1, color);  // Diagonal line: top-left to bottom-right
    cvs->draw_line(x0, y1, x1, y0, color);  // Diagonal line: bottom-left to top-right
}

void draw_window_decorations(kstl::shared_ptr<canvas>& cvs, window* win, 
    color_t border_color, color_t title_bar_color, 
    color_t text_color) {
    // Window properties
    int border_thickness = 2;
    int title_bar_height = 24;
    int x = win->xpos - border_thickness;
    int y = win->ypos - border_thickness;
    int width = win->width + border_thickness * 2;
    int height = win->height + border_thickness * 2 + title_bar_height;

    // Draw the window border
    cvs->draw_rect(x, y, width, height, border_color);

    // Fill the title bar background
    cvs->fill_rect(x + border_thickness, y + border_thickness, 
    width - 2 * border_thickness, title_bar_height, title_bar_color);

    // Draw the close button (a small square on the right of the title bar)
    int close_button_size = title_bar_height - 8;  // Slight padding from edges
    int close_button_x = x + width - border_thickness - close_button_size - 2;  // Right-aligned
    int close_button_y = y + border_thickness + 4;  // Slight padding from top

    cvs->fill_rect(close_button_x, close_button_y, close_button_size, close_button_size, border_color);
    draw_close_button_icon(cvs, close_button_x, close_button_y, close_button_size, text_color);

    // Draw the window title
    const char* title = win->title.c_str();
    int text_x = x + border_thickness + 8;  // Small left padding
    int text_y = y + (title_bar_height - 10) / 2;  // Center vertically
    cvs->draw_string(text_x, text_y, title, text_color);

    // Draw the title border
    cvs->draw_rect(x, y, width, title_bar_height + border_thickness * 2, border_color);
}

int main() {
    // Initialize the screen manager
    auto screen = kstl::make_shared<screen_manager>();
    if (!screen->initialize()) {
        return -1;
    }

    // Retrieve the primary screen canvas
    auto cvs = screen->get_canvas();

    char cpu_vendor_str[16] = { 0 };
    arch::x86::cpuid_read_vendor_id(cpu_vendor_str);

    char time_str_buf[128] = { 0 };
    char cpu_vendor_display_str_buf[128] = { 0 };
    sprintf(cpu_vendor_display_str_buf, 127, "CPU: %s", cpu_vendor_str);

    auto app = kstl::make_shared<sample_window_app>();
    app->init();

    while (true) {
        uint64_t sys_uptime = kernel_timer::get_system_time_in_seconds();
        zeromem(time_str_buf, 127);

        uint64_t hours = sys_uptime / 3600;
        uint64_t minutes = (sys_uptime % 3600) / 60;
        uint64_t seconds = sys_uptime % 60;

        sprintf(time_str_buf, 127, "System Uptime: %lluh %llum %llus", hours, minutes, seconds);

        app->render();

        screen->begin_frame();

        cvs->draw_string(cvs->width() - 220, 2, time_str_buf, 0xffffffff);
        cvs->draw_string(16, 2, cpu_vendor_display_str_buf, 0xffffffff);

        for (auto window : screen->get_all_windows()) {
            auto window_canvas = window->get_canvas().get();
            
            // Draw window decorations (border, title bar, close button)
            draw_window_decorations(cvs, window, 0xff000000, 0xff333333, 0xffffffff);

            // Composite the window contents
            cvs->composite_canvas(window->xpos, window->ypos + 24, window_canvas);  // Offset for title bar
        }

        draw_cursor(cvs, g_mouse_cursor_pos_x, g_mouse_cursor_pos_y, 0x00000000, 0xffffffff);

        screen->end_frame();

        // ~16 ms == ~60 FPS
        msleep(8);
    }

    return 0;
}
