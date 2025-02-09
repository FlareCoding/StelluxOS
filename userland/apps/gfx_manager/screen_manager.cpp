#include "screen_manager.h"
#include <fs/vfs.h>

psf1_font* screen_manager::s_global_font = nullptr;
kstl::vector<window*> screen_manager::s_window_registry;

void screen_manager::register_window(window* wnd) {
    s_window_registry.push_back(wnd);
}

screen_manager::screen_manager()
    : m_gfx_module(nullptr), m_canvas(nullptr) {}

screen_manager::~screen_manager() {
    // TO-DO: resource clean-up
}

psf1_font* screen_manager::get_global_system_font() {
    return s_global_font;
}

bool screen_manager::initialize() {
    s_global_font = _load_font();
    if (!s_global_font) {
        return false;
    }
    
    if (!_create_canvas(s_global_font)) {
        return false;
    }

    return true;
}

kstl::shared_ptr<canvas> screen_manager::get_canvas() const {
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
    framebuffer_t fb;
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
    m_canvas = kstl::make_shared<canvas>(fb, font);

#if 0
    serial::printf(
        "screen_manager: Successfully initialized canvas: %ux%u pitch=%u bpp=%u\n",
        fb.width, fb.height, fb.pitch, fb.bpp
    );
#endif

    return true;
}

psf1_font* screen_manager::_load_font() {
    auto& vfs = fs::virtual_filesystem::get();
    const kstl::string font_filepath = "/initrd/res/fonts/zap-light16.psf";

    if (!vfs.path_exists(font_filepath)) {
        serial::printf("[!] screen_manager: failed to load zap-light16.psf text font, file not found\n");
        return nullptr;
    }

    fs::vfs_stat_struct stat;
    if (vfs.stat(font_filepath, stat) != fs::fs_error::success) {
        serial::printf("[!] screen_manager: failed to stat font file\n");
        return nullptr;
    }

    // Allocate a buffer for the font data
    psf1_font* font = reinterpret_cast<psf1_font*>(zmalloc(stat.size));
    if (!font) {
        serial::printf("[!] screen_manager: failed to allocate font buffer\n");
        return nullptr;
    }

    // Read in the font data from the file
    ssize_t bytes_read = vfs.read(font_filepath, font, stat.size, 0);
    if (bytes_read != static_cast<ssize_t>(stat.size)) {
        serial::printf("[!] screen_manager: failed to read font file into buffer\n");
        return nullptr;
    }

    // Verify the PSF font magic number
    if (font->header.magic[0] != 0x36 || font->header.magic[1] != 0x04) {
        serial::printf("[!] screen_manager: PSF font magic number was\n");
        return nullptr;
    }

    // Initialize helper data in the font
    font->width = 8;
    font->height = font->header.char_height;
    font->glyph_count = (font->header.mode & 0x01) ? 512 : 256;
    font->glyph_data = reinterpret_cast<const uint8_t*>(font) + sizeof(psf1_font_hdr);

    return font;
}

