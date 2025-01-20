#include "screen_manager.h"
#include <arch/x86/cpuid.h>

static uint32_t get_random_color() {
    static uint32_t seed = 1;
    seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
    uint8_t r = (seed >> 16) & 0xFF;
    uint8_t g = (seed >>  8) & 0xFF;
    uint8_t b = (seed      ) & 0xFF;
    return 0xFF000000 | (r << 16) | (g << 8) | b; // ARGB
}

int main() {
    // Initialize the screen manager
    auto screen = kstl::make_shared<screen_manager>();
    if (!screen->initialize()) {
        return -1;
    }

    // Retrieve the primary screen canvas
    auto cvs = screen->get_canvas();

    // Rectangle parameters
    int rect_x = 10;
    int rect_y = 10;
    int rect_w = 50;
    int rect_h = 50;

    // Velocity (pixels per frame)
    int dx = 4;
    int dy = 4;

    // Initial color
    uint32_t rect_color = 0xFFFF0000; // ARGB (bright red)

    char cpu_vendor_str[16] = { 0 };
    arch::x86::cpuid_read_vendor_id(cpu_vendor_str);

    char time_str_buf[128] = { 0 };
    char cpu_vendor_display_str_buf[128] = { 0 };
    sprintf(cpu_vendor_display_str_buf, 127, "CPU: %s", cpu_vendor_str);

    while (true) {
        uint64_t sys_uptime = kernel_timer::get_system_time_in_seconds();
        zeromem(time_str_buf, 127);
        sprintf(time_str_buf, 127, "System Uptime: %um %us", sys_uptime / 60, sys_uptime % 60);

        screen->begin_frame();

        cvs->draw_string(cvs->width() - 200, 2, time_str_buf, 0xffffffff);
        cvs->draw_string(16, 2, cpu_vendor_display_str_buf, 0xffffffff);
        cvs->fill_rect(rect_x, rect_y, rect_w, rect_h, rect_color);

        screen->end_frame();

        // Update the rectangle position
        rect_x += dx;
        rect_y += dy;

        // Horizontal boundaries
        if (rect_x < 0) {
            rect_x = 0;
            dx = -dx;
            rect_color = get_random_color();
        } else if ((rect_x + rect_w) >= (int)cvs->width()) {
            rect_x = cvs->width() - rect_w;
            dx = -dx;
            rect_color = get_random_color();
        }

        // Vertical boundaries
        if (rect_y < 0) {
            rect_y = 0;
            dy = -dy;
            rect_color = get_random_color();
        } else if ((rect_y + rect_h) >= (int)cvs->height()) {
            rect_y = cvs->height() - rect_h;
            dy = -dy;
            rect_color = get_random_color();
        }

        // ~16 ms == ~60 FPS
        msleep(8);
    }

    return 0;
}
