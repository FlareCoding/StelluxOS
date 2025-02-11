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

    sleep(1);

    if (!stella_ui::render_content()) {
        serial::printf("[EXAMPLE_APP] Failed to render content\n");
        return -1;
    }

    return 0;
}
