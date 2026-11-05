#include "drivers/graphics/nvidia/display_registration.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"

namespace drivers::nvidia {

// Logs `output` of `display` as gfxfb reads it
static void log_registered_output(gfxfb::display& display, uint32_t output) {
    gfxfb::output_state state = {};
    display.read_output(output, state);
    const char* boot = state.boot_output ? "the boot firmware lit" : "the boot firmware did not light";

    const gfxfb::buffer* base = display.base_buffer(output);
    if (!state.lit || base == nullptr) {
        log::info("nvidia: gfxfb reads output %u, an output %s, as dark", output, boot);
        return;
    }

    uint64_t millihertz = state.current.refresh_millihertz;
    const char* unit = base->height == 1 ? "row" : "rows";
    log::info(
        "nvidia: gfxfb reads output %u, an output %s, as lit at %u by %u at %lu.%03lu Hz with its base buffer at "
        "physical address 0x%lX in BAR1, %u %s of %u bytes",
        output,
        boot,
        state.current.width,
        state.current.height,
        millihertz / 1000,
        millihertz % 1000,
        base->phys,
        base->height,
        unit,
        base->pitch
    );
}

int32_t register_display_engine(
    display_engine& engine,
    bool is_boot_display_shown,
    register_display_call register_display
) {
    if (is_boot_display_shown && !engine.has_lit_output()) {
        log::info(
            "nvidia: the display engine has lit no output, so it is not registered and gfxfb keeps the boot "
            "framebuffer"
        );
        return OK;
    }

    int32_t rc = register_display(&engine);
    if (rc != gfxfb::OK) {
        log::error("nvidia: gfxfb refused the display engine as its display");
        return ERR_NOT_REGISTERED;
    }

    engine.mark_registered();

    uint32_t output_count = engine.output_count();
    const char* unit = output_count == 1 ? "output" : "outputs";
    log::info("nvidia: the display engine is gfxfb's display, of %u %s", output_count, unit);
    for (uint32_t output = 0; output < output_count; output++) {
        log_registered_output(engine, output);
    }

    return OK;
}

} // namespace drivers::nvidia
