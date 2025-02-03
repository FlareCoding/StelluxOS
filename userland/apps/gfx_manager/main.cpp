#include "screen_manager.h"
#include <arch/x86/cpuid.h>

extern uint64_t g_mouse_cursor_pos_x;
extern uint64_t g_mouse_cursor_pos_y;

void draw_cursor(kstl::shared_ptr<canvas>& cvs, int x, int y, uint32_t color) {
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
                cvs->fill_rect(x + col, y + row, 1, 1, color);
            }
        }
    }
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

    while (true) {
        uint64_t sys_uptime = kernel_timer::get_system_time_in_seconds();
        zeromem(time_str_buf, 127);

        uint64_t hours = sys_uptime / 3600;
        uint64_t minutes = (sys_uptime % 3600) / 60;
        uint64_t seconds = sys_uptime % 60;

        sprintf(time_str_buf, 127, "System Uptime: %lluh %llum %llus", hours, minutes, seconds);

        screen->begin_frame();

        cvs->draw_string(cvs->width() - 220, 2, time_str_buf, 0xffffffff);
        cvs->draw_string(16, 2, cpu_vendor_display_str_buf, 0xffffffff);

        draw_cursor(cvs, g_mouse_cursor_pos_x, g_mouse_cursor_pos_y, 0xffffffff);

        screen->end_frame();

        // ~16 ms == ~60 FPS
        msleep(8);
    }

    return 0;
}
