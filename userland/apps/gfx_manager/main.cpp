#include "screen_manager.h"
#include <arch/x86/cpuid.h>
#include <ipc/mq.h>

#include "sample_window_app.h"

extern uint64_t g_mouse_cursor_pos_x;
extern uint64_t g_mouse_cursor_pos_y;
extern bool g_mouse_button_pressed;

struct test_msg {
    uint64_t secret;
};

void draw_cursor(kstl::shared_ptr<stella_ui::canvas>& cvs, int x, int y, uint32_t fill_color, uint32_t border_color) {
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

int main() {
    // Initialize the screen manager
    auto screen = kstl::make_shared<screen_manager>();
    if (!screen->initialize()) {
        return -1;
    }

    auto queue = ipc::message_queue::create("gfx_manager_mq");

    // Retrieve the primary screen canvas
    auto cvs = screen->get_screen_canvas();

    char cpu_vendor_str[16] = { 0 };
    arch::x86::cpuid_read_vendor_id(cpu_vendor_str);

    char time_str_buf[128] = { 0 };
    char cpu_vendor_display_str_buf[128] = { 0 };
    sprintf(cpu_vendor_display_str_buf, 127, "CPU: %s", cpu_vendor_str);

    auto app_window = kstl::make_shared<example_window>();
    app_window->setup();

    while (true) {
        uint64_t sys_uptime = kernel_timer::get_system_time_in_seconds();
        zeromem(time_str_buf, 127);

        uint64_t hours = sys_uptime / 3600;
        uint64_t minutes = (sys_uptime % 3600) / 60;
        uint64_t seconds = sys_uptime % 60;

        sprintf(time_str_buf, 127, "System Uptime: %lluh %llum %llus", hours, minutes, seconds);

        if (g_mouse_button_pressed) {
            app_window->position.x = g_mouse_cursor_pos_x - 60;
            app_window->position.y = g_mouse_cursor_pos_y - 6;
        }

        app_window->draw();

        screen->begin_frame();

        cvs->draw_string(cvs->width() - 220, 2, time_str_buf, 0xffffffff);
        cvs->draw_string(16, 2, cpu_vendor_display_str_buf, 0xffffffff);

        app_window->draw_decorations(cvs);

        auto window_canvas = app_window->get_canvas().get();
        auto window_canvas_pos = app_window->get_canvas_position();
        cvs->composite_canvas(window_canvas_pos.x, window_canvas_pos.y, window_canvas);

        draw_cursor(cvs, g_mouse_cursor_pos_x, g_mouse_cursor_pos_y, 0x00000000, 0xffffffff);

        screen->end_frame();

        // ~16 ms == ~60 FPS
        msleep(8);

        if (ipc::message_queue::peek_message(queue)) {
            ipc::mq_message msg;
            ipc::message_queue::get_message(queue, &msg);

            test_msg* content = reinterpret_cast<test_msg*>(msg.payload);

            serial::printf("[GFX_MANAGER] Message received: %llu, secret: %llu\n", msg.message_id, content->secret);
        }
    }

    return 0;
}
