#include "screen_manager.h"
#include <arch/x86/cpuid.h>

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

        screen->end_frame();

        // ~16 ms == ~60 FPS
        msleep(8);
    }

    return 0;
}
