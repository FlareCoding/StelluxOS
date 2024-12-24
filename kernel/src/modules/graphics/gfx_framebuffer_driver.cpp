#include <modules/graphics/gfx_framebuffer_driver.h>
#include <serial/serial.h>
#include <dynpriv/dynpriv.h>

namespace modules {
gfx_framebuffer_driver::gfx_framebuffer_driver(
    uintptr_t physbase,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint8_t bpp
) : module_base("gfx_framebuffer_driver") {
    __unused physbase;
    __unused width;
    __unused height;
    __unused pitch;
    __unused bpp;
}

bool gfx_framebuffer_driver::init() {
    serial::printf("gfx_framebuffer_driver::init()\n");
    return true;
}

bool gfx_framebuffer_driver::start() {
    serial::printf("gfx_framebuffer_driver::start()\n");
    return true;
}

bool gfx_framebuffer_driver::stop() {
    serial::printf("gfx_framebuffer_driver::stop()\n");
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
