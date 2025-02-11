#include "screen_manager.h"

int main() {
    // Initialize the screen manager
    auto screen = kstl::make_shared<screen_manager>();
    if (!screen->initialize()) {
        return -1;
    }

    while (true) {
        screen->poll_events();
        screen->begin_frame();

        screen->composite_windows();
        screen->draw_screen_overlays();

        screen->end_frame();
        screen->send_paint_notifications();

        // ~16 ms == ~60 FPS
        msleep(8);
    }

    return 0;
}
