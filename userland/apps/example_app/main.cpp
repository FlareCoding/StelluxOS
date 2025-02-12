#include <ipc/mq.h>
#include <serial/serial.h>
#include <time/time.h>
#include <process/process.h>

#include <stella_user.h>

extern bool g_mouse_button_pressed;

int main() {
    if (!stella_ui::connect_to_compositor()) {
        serial::printf("[EXAMPLE_APP] Failed to connect to compositor\n");
        return -1;
    }

    serial::printf("[EXAMPLE_APP] Connected to compositor!\n");
    sleep(2);

    if (!stella_ui::create_window(400, 300, "Example App")) {
        serial::printf("[EXAMPLE_APP] Failed to create a window\n");
        return -1;
    }

    kstl::shared_ptr<stella_ui::canvas> canvas;
    if (!stella_ui::request_map_window_canvas(canvas)) {
        serial::printf("[EXAMPLE_APP] Failed to map window canvas\n");
        return -1;
    }

    canvas->set_background_color(stella_ui::color::dark_gray.to_argb());

    char pid_str[64] = { 0 };
    sprintf(pid_str, 63, "pid: %u", current->pid);

    char click_count_str[64] = { 0 };
    int click_count = 0;

    canvas->clear();
    canvas->draw_string(20, 20, pid_str, stella_ui::color::green.to_argb());
    canvas->draw_string(20, 50, "Clicks: 0", stella_ui::color::white.to_argb());

    bool last_mouse_state = false;

    while (true) {
        if (g_mouse_button_pressed && !last_mouse_state) {
            click_count++;
            sprintf(click_count_str, 63, "Clicks: %u", click_count);
            canvas->clear();
            canvas->draw_string(20, 20, pid_str, stella_ui::color::green.to_argb());
            canvas->draw_string(20, 50, click_count_str, stella_ui::color::white.to_argb());
        }

        last_mouse_state = g_mouse_button_pressed;
        sched::yield();
    }

    return 0;
}
