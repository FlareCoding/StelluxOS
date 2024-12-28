#include <modules/graphics/gfx_framebuffer_driver.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <dynpriv/dynpriv.h>

namespace modules {
gfx_framebuffer_driver::gfx_framebuffer_driver(
    uintptr_t physbase,
    const framebuffer_t& framebuffer
) : module_base("gfx_framebuffer_driver") {
    m_physical_base = physbase;
    m_native_hw_buffer = framebuffer;
}

bool gfx_framebuffer_driver::init() {
    // Calculate how many pages the framebuffer needs
    uint32_t page_count = (m_native_hw_buffer.pitch * m_native_hw_buffer.height) / PAGE_SIZE + 1;

    RUN_ELEVATED({
        m_native_hw_buffer.pixels = reinterpret_cast<uint8_t*>(
            vmm::map_contiguous_physical_pages(m_physical_base, page_count, DEFAULT_UNPRIV_PAGE_FLAGS | PTE_PAT)
        );
    });

    if (!m_native_hw_buffer.pixels) {
        return false;
    }

    // Clear the screen (FOR TESTING)
    for (uint32_t i = 0; i < m_native_hw_buffer.pitch * m_native_hw_buffer.height; i++) {
        m_native_hw_buffer.pixels[i] = 0x1F;
    }

    // Helper to fill rectangles
    auto fill_rect = [&](int x, int y, int width, int height, uint32_t color) {
        for (int row = y; row < y + height; ++row) {
            for (int col = x; col < x + width; ++col) {
                int offset = (row * m_native_hw_buffer.pitch) + (col * 3); // Assuming 3 bytes per pixel (RGB)
                m_native_hw_buffer.pixels[offset + 0] = (color >> 0) & 0xFF;  // Blue
                m_native_hw_buffer.pixels[offset + 1] = (color >> 8) & 0xFF;  // Green
                m_native_hw_buffer.pixels[offset + 2] = (color >> 16) & 0xFF; // Red
            }
        }
    };

    // Define drawing parameters
    const int start_x = 100, start_y = 100; // Top-left corner of the drawing region
    const int letter_width = 50, letter_height = 100;
    const int letter_spacing = 20; // Spacing between letters
    const uint32_t text_color = 0xFFFFFF; // White

    // Draw "StelluxOS" in a very simple blocky style
    int cursor_x = start_x;

    // S
    fill_rect(cursor_x, start_y, letter_width, 20, text_color);                           // Top
    fill_rect(cursor_x, start_y, 20, letter_height / 2, text_color);                      // Top left
    fill_rect(cursor_x, start_y + (letter_height / 2) - 10, letter_width, 20, text_color); // Middle
    fill_rect(cursor_x + letter_width - 20, start_y + (letter_height / 2), 20, letter_height / 2, text_color); // Bottom right
    fill_rect(cursor_x, start_y + letter_height - 20, letter_width, 20, text_color);     // Bottom
    cursor_x += letter_width + letter_spacing;

    // T
    fill_rect(cursor_x, start_y, letter_width, 20, text_color);                           // Top
    fill_rect(cursor_x + (letter_width / 2) - 10, start_y, 20, letter_height, text_color); // Vertical bar
    cursor_x += letter_width + letter_spacing;

    // E
    fill_rect(cursor_x, start_y, letter_width, 20, text_color);                           // Top
    fill_rect(cursor_x, start_y, 20, letter_height, text_color);                          // Vertical bar
    fill_rect(cursor_x, start_y + (letter_height / 2) - 10, letter_width, 20, text_color); // Middle
    fill_rect(cursor_x, start_y + letter_height - 20, letter_width, 20, text_color);     // Bottom
    cursor_x += letter_width + letter_spacing;

    // L
    fill_rect(cursor_x, start_y, 20, letter_height, text_color);                          // Vertical bar
    fill_rect(cursor_x, start_y + letter_height - 20, letter_width, 20, text_color);     // Bottom
    cursor_x += letter_width + letter_spacing;

    // L (second L)
    fill_rect(cursor_x, start_y, 20, letter_height, text_color);                          // Vertical bar
    fill_rect(cursor_x, start_y + letter_height - 20, letter_width, 20, text_color);     // Bottom
    cursor_x += letter_width + letter_spacing;

    // U
    fill_rect(cursor_x, start_y, 20, letter_height, text_color);                          // Left bar
    fill_rect(cursor_x + letter_width - 20, start_y, 20, letter_height, text_color);     // Right bar
    fill_rect(cursor_x, start_y + letter_height - 20, letter_width, 20, text_color);     // Bottom
    cursor_x += letter_width + letter_spacing;

    // X
    for (int i = 0; i < letter_height; i++) {
        // Left-to-right diagonal
        int left_to_right_x = cursor_x + (i * letter_width) / letter_height;
        fill_rect(left_to_right_x, start_y + i, 5, 5, text_color);

        // Right-to-left diagonal
        int right_to_left_x = cursor_x + letter_width - (i * letter_width) / letter_height - 5;
        fill_rect(right_to_left_x, start_y + i, 5, 5, text_color);
    }
    cursor_x += letter_width + letter_spacing;

    // O
    fill_rect(cursor_x, start_y, letter_width, 20, text_color);                           // Top
    fill_rect(cursor_x, start_y, 20, letter_height, text_color);                          // Left bar
    fill_rect(cursor_x + letter_width - 20, start_y, 20, letter_height, text_color);     // Right bar
    fill_rect(cursor_x, start_y + letter_height - 20, letter_width, 20, text_color);     // Bottom
    cursor_x += letter_width + letter_spacing;

    // S (same as above)
    fill_rect(cursor_x, start_y, letter_width, 20, text_color);                           // Top
    fill_rect(cursor_x, start_y, 20, letter_height / 2, text_color);                      // Top left
    fill_rect(cursor_x, start_y + (letter_height / 2) - 10, letter_width, 20, text_color); // Middle
    fill_rect(cursor_x + letter_width - 20, start_y + (letter_height / 2), 20, letter_height / 2, text_color); // Bottom right
    fill_rect(cursor_x, start_y + letter_height - 20, letter_width, 20, text_color);     // Bottom

    return true;
}

bool gfx_framebuffer_driver::start() {
    return true;
}

bool gfx_framebuffer_driver::stop() {
    return true;
}

bool gfx_framebuffer_driver::on_command(
    uint64_t  command,
    const void* data_in,
    size_t      data_in_size,
    void*       data_out,
    size_t      data_out_size
) {
    __unused command;
    __unused data_in;
    __unused data_in_size;
    __unused data_out;
    __unused data_out_size;
    return true;
}
} // namespace modules
