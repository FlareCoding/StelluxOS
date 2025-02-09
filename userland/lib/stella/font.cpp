#include "font.h"
#include <fs/vfs.h>
#include <serial/serial.h>

namespace stella_ui {
psf1_font* _load_system_font() {
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
} // namespace stella_ui
