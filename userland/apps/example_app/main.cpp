#include <ipc/mq.h>
#include <serial/serial.h>
#include <time/time.h>

#include <stella_user.h>

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

    return 0;
}
