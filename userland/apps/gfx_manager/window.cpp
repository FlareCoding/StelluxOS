#include "window.h"
#include "screen_manager.h"

uint64_t g_available_window_id = 1;

window* window::create_window(
    uint32_t width,
    uint32_t height,
    const char* title
) {
    window* wnd = new window();
    if (!wnd) {
        return nullptr;
    }

    wnd->width = width;
    wnd->height = height;
    wnd->title = title;

    wnd->id = g_available_window_id++;
    if (!wnd->init_graphics_ctx()) {
        return nullptr;
    }

    screen_manager::register_window(wnd);
    return wnd;
}

window::window() {}

bool window::init_graphics_ctx() {
    framebuffer_t fb;
    fb.width = width;
    fb.height = height;
    fb.bpp = 32;
    fb.pitch = width * (fb.bpp / 8);
    fb.data = reinterpret_cast<uint8_t*>(zmalloc(fb.pitch * fb.height));

    if (!fb.data) {
        serial::printf("[!] Failed to create a window\n");
        return false;
    }

    psf1_font* font = screen_manager::get_global_system_font();
    m_canvas = kstl::make_shared<canvas>(fb, font);

    return true;
}
