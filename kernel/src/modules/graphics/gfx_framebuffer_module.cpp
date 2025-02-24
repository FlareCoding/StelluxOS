#include <modules/graphics/gfx_framebuffer_module.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <dynpriv/dynpriv.h>
#include <time/time.h>

namespace modules {
gfx_framebuffer_module::gfx_framebuffer_module(
    uintptr_t physbase,
    const framebuffer_t& framebuffer
) : module_base("gfx_framebuffer_module") {
    m_physical_base = physbase;
    m_native_hw_buffer = framebuffer;
}

bool gfx_framebuffer_module::init() {
    // Calculate how many pages the framebuffer needs
    uint32_t page_count = (m_native_hw_buffer.pitch * m_native_hw_buffer.height) / PAGE_SIZE + 1;

    // Initialize the front buffer
    RUN_ELEVATED({
        m_native_hw_buffer.data = reinterpret_cast<uint8_t*>(
            vmm::map_contiguous_physical_pages(m_physical_base, page_count, DEFAULT_UNPRIV_PAGE_FLAGS | PTE_PAT)
        );
    });

    if (!m_native_hw_buffer.data) {
        return false;
    }

    // Initialize the back buffer
    memcpy(&m_back_buffer, &m_native_hw_buffer, sizeof(framebuffer_t));
    RUN_ELEVATED({
        m_back_buffer.data = reinterpret_cast<uint8_t*>(
            vmm::alloc_contiguous_virtual_pages(page_count, DEFAULT_UNPRIV_PAGE_FLAGS)
        );
    });

    // Start with a cleared screen
    clear_screen(0x22);
    swap_buffers();

    return true;
}

bool gfx_framebuffer_module::start() {
    return true;
}

bool gfx_framebuffer_module::stop() {
    return true;
}

bool gfx_framebuffer_module::on_command(
    uint64_t  command,
    const void* data_in,
    size_t      data_in_size,
    void*       data_out,
    size_t      data_out_size
) {
    switch (command) {
    case CMD_CLEAR_SCREEN: {
        // If a color byte is provided, use it
        uint8_t color = 0x00;
        if (data_in && data_in_size == sizeof(uint8_t)) {
            color = *reinterpret_cast<const uint8_t*>(data_in);
        }
        clear_screen(color);
        return true;
    }
    case CMD_SWAP_BUFFERS: {
        swap_buffers();
        return true;
    }
    case CMD_MAP_BACKBUFFER: {
        if (data_out && data_out_size == sizeof(framebuffer_t)) {
            memcpy(reinterpret_cast<framebuffer_t*>(data_out), &m_back_buffer, sizeof(framebuffer_t));
        }
        return true;
    }
    default: {
        // Unknown command
        return false;
    }
    }
}

void gfx_framebuffer_module::clear_screen(uint8_t color) {
    // Clear both front and back buffers to maintain sync
    uint32_t fb_size = m_native_hw_buffer.pitch * m_native_hw_buffer.height;
    memset(m_back_buffer.data,      color, fb_size);
}

void gfx_framebuffer_module::swap_buffers() {
    // Simple CPU copy from back -> front
    uint32_t fb_size = m_native_hw_buffer.pitch * m_native_hw_buffer.height;
    memcpy(m_native_hw_buffer.data, m_back_buffer.data, fb_size);
}
} // namespace modules
