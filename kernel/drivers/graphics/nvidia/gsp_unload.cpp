// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/gsp_unload.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"

namespace drivers::nvidia {

namespace {

// The arguments NVIDIA's driver sends with the request, in the layout of version 0x1F07, which it takes from RPC
// version 0x1F070000 on and so for this firmware, run at the RPC version made from VGX version 0x23.05
struct unload_arguments {
    uint8_t  in_power_transition; // 0, as the unload is no suspend for power management
    uint8_t  entering_gc6;        // 0, as the GPU enters no deep sleep state
    uint8_t  padding[2];
    uint32_t new_power_level;     // 0
};

static_assert(sizeof(unload_arguments) == 8, "NVIDIA's layout of the unload's arguments");

} // namespace

int32_t unload_gsp_firmware(
    gsp& conversation,
    const registers& regs,
    uint64_t reply_timeout_ns,
    uint64_t suspend_timeout_ns
) {
    unload_arguments arguments = {};
    received_message reply = {};

    int32_t request_rc = conversation.request(
        FUNCTION_UNLOADING_GUEST_DRIVER,
        &arguments,
        sizeof(arguments),
        0,
        reply,
        reply_timeout_ns
    );
    int32_t suspend_rc = wait_for_gsp_suspend(regs, suspend_timeout_ns);
    if (request_rc != OK) {
        return request_rc;
    }

    return suspend_rc;
}

} // namespace drivers::nvidia
