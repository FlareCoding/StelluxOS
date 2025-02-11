#include <ipc/mq.h>
#include <serial/serial.h>
#include <time/time.h>

#include <stella_user.h>

void process_event(stella_ui::compositor_event evt) {
    switch (evt) {
    case stella_ui::compositor_event::comp_evt_paint: {
        stella_ui::render_content();
        break;
    }
    default: {
        break;
    }
    }
}

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

    while (true) {
        while (stella_ui::peek_compositor_events()) {
            auto evt = stella_ui::get_compositor_event();
            process_event(evt);
        }

        msleep(8);
    }    

    return 0;
}
