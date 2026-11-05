#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_REGISTRATION_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_REGISTRATION_H

#include "common/types.h"
#include "drivers/graphics/gfxfb.h"
#include "drivers/graphics/nvidia/display_engine.h"

namespace drivers::nvidia {

// gfxfb's registration call, `gfxfb::register_display` run elevated or a test's stand-in, returning a gfxfb result
using register_display_call = int32_t (*)(gfxfb::display* display);

/**
 * @brief Register `engine` with gfxfb through `register_display` and log each
 * output as gfxfb reads it, unless the engine has lit no output while gfxfb
 * shows the boot framebuffer, which then stays userland's, logging that the
 * engine is not registered. With no boot framebuffer the engine registers
 * though it has lit no output, every output reading as dark. It must be called
 * from the driver's `attach()` before userland starts, as gfxfb requires, and
 * holding no lock, since gfxfb's reads take the engine's lock. The engine is
 * marked registered once `register_display` accepts it.
 * @param engine The set-up display engine. Once registered it must never be
 * freed, since gfxfb keeps it.
 * @param is_boot_display_shown Whether gfxfb shows the boot framebuffer.
 * @param register_display gfxfb's registration call.
 * @return `OK` once the engine is registered or left unregistered while gfxfb
 * keeps the boot framebuffer, else `ERR_NOT_REGISTERED`, logged, when gfxfb
 * refuses it.
 */
int32_t register_display_engine(
    display_engine& engine,
    bool is_boot_display_shown,
    register_display_call register_display
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_REGISTRATION_H
