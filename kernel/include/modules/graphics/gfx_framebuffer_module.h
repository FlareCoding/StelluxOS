#ifndef GFX_FRAMEBUFFER_MODULE_H
#define GFX_FRAMEBUFFER_MODULE_H
#include <modules/module_manager.h>

namespace modules {
class gfx_framebuffer_module : public module_base {
public:
    struct framebuffer_t {
        uint32_t    width;
        uint32_t    height;
        uint32_t    pitch;
        uint8_t     bpp;
        uint8_t*    data;
    };

    enum command_id : uint64_t {
        CMD_CLEAR_SCREEN    = 0x01,
        CMD_SWAP_BUFFERS    = 0x02,
        CMD_MAP_BACKBUFFER  = 0x03
    };

    explicit gfx_framebuffer_module(
        uintptr_t physbase,
        const framebuffer_t& framebuffer
    );

    // ------------------------------------------------------------------------
    // Lifecycle Hooks (overrides from module_base)
    // ------------------------------------------------------------------------
    
    bool init() override;
    bool start() override;
    bool stop() override;

    // ------------------------------------------------------------------------
    // Command Interface
    // ------------------------------------------------------------------------

    bool on_command(
        uint64_t  command,
        const void* data_in,
        size_t      data_in_size,
        void*       data_out,
        size_t      data_out_size
    ) override;

    // ------------------------------------------------------------------------
    // Graphics Syscall Support
    // ------------------------------------------------------------------------
    
    /**
     * @brief Get framebuffer information for syscall interface
     * @param info Pointer to framebuffer info structure to fill
     * @return true on success, false on error
     */
    bool get_framebuffer_info(void* info) const;
    
    /**
     * @brief Get the physical address of the framebuffer
     * @return Physical address of the framebuffer
     */
    uintptr_t get_physical_address() const { return m_physical_base; }
    
    /**
     * @brief Get the total size of the framebuffer in bytes
     * @return Size in bytes
     */
    size_t get_total_fb_size() const;

private:
    uintptr_t       m_physical_base;
    framebuffer_t   m_native_hw_buffer;
    framebuffer_t   m_back_buffer;

    void clear_screen(uint8_t color);
    void swap_buffers();
};

} // namespace modules

#endif // GFX_FRAMEBUFFER_MODULE_H

