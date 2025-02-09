#include "screen_manager.h"

kstl::vector<stella_ui::window_base*> screen_manager::s_window_registry;

void screen_manager::register_window(stella_ui::window_base* wnd) {
    s_window_registry.push_back(wnd);
}

screen_manager::screen_manager()
    : m_gfx_module(nullptr), m_canvas(nullptr) {}

screen_manager::~screen_manager() {
    // TO-DO: resource clean-up
}

bool screen_manager::initialize() {
    psf1_font* font = stella_ui::_load_system_font();
    if (!font) {
        return false;
    }
    
    if (!_create_canvas(font)) {
        return false;
    }

    return true;
}

kstl::shared_ptr<stella_ui::canvas> screen_manager::get_screen_canvas() const {
    return m_canvas;
}

void screen_manager::begin_frame() {
    m_canvas->clear();
}

void screen_manager::end_frame() {
    if (!m_gfx_module) {
        return;
    }

    auto& mgr = modules::module_manager::get();
    mgr.send_command(
        m_gfx_module,
        modules::gfx_framebuffer_module::CMD_SWAP_BUFFERS,
        nullptr, 0, nullptr, 0
    );
}

bool screen_manager::_create_canvas(psf1_font* font) {
    // Find the graphics module
    auto& mgr = modules::module_manager::get();
    m_gfx_module = mgr.find_module("gfx_framebuffer_module");
    if (!m_gfx_module) {
        serial::printf("screen_manager: Failed to find gfx_framebuffer_module\n");
        return false;
    }

    // Wait until it's running
    while (m_gfx_module->state() != modules::module_state::running) {
        msleep(100);
    }

    // Request to map the back buffer
    stella_ui::framebuffer_t fb;
    bool result = mgr.send_command(
        m_gfx_module,
        modules::gfx_framebuffer_module::CMD_MAP_BACKBUFFER,
        nullptr, 0,
        &fb, sizeof(fb)
    );

    if (!result || !fb.data) {
        serial::printf("[!] screen_manager: Failed to map back buffer.\n");
        return false;
    }

    // Create the canvas
    m_canvas = kstl::make_shared<stella_ui::canvas>(fb, font);

#if 0
    serial::printf(
        "screen_manager: Successfully initialized canvas: %ux%u pitch=%u bpp=%u\n",
        fb.width, fb.height, fb.pitch, fb.bpp
    );
#endif

    return true;
}

