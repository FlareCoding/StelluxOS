#include "screen_manager.h"

__PRIVILEGED_DATA
extern char* g_mbi_kernel_cmdline;

int main() {
    // Initialize the screen manager
    auto screen = kstl::make_shared<screen_manager>();
    if (!screen->initialize()) {
        return -1;
    }

    bool console_mode = false;
    RUN_ELEVATED({
        kstl::string cmdline_args = kstl::string(g_mbi_kernel_cmdline);
        if (cmdline_args.find("gfxmode=console") != kstl::string::npos) {
            console_mode = true;
        }
    });

    screen->active_mode = console_mode ? screen_manager_mode::console : screen_manager_mode::compositor;

    while (true) {
        screen->poll_events();

        if (screen->active_mode == screen_manager_mode::console) {
            screen->set_background_color(stella_ui::color(20, 20, 20));
            screen->begin_frame();

            screen->draw_kernel_log_console();
            
            screen->end_frame();
            msleep(128);
        } else if (screen->active_mode == screen_manager_mode::compositor) {
            screen->set_background_color(stella_ui::color(0xff222222));
            screen->begin_frame();

            screen->composite_windows();
            screen->draw_screen_overlays();

            screen->end_frame();

            // ~16 ms == ~60 FPS
            msleep(8);
        }
    }

    return 0;
}
