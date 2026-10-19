// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/gsp_info.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "common/string.h"

namespace drivers::nvidia {

// The room NVIDIA gives each string of the guest system information, the arguments of `FUNCTION_SET_GUEST_SYSTEM_INFO`
constexpr uint32_t GUEST_STRING_BYTES = 256;

namespace {

// The guest system information in NVIDIA's layout, which the reply carries back with the version the firmware accepts
struct guest_system_info {
    uint32_t vgx_version_major;
    uint32_t vgx_version_minor;
    uint32_t driver_version_bytes;
    uint32_t branch_bytes;
    uint32_t title_bytes;
    uint32_t changelist;
    char     driver_version[GUEST_STRING_BYTES];
    char     branch[GUEST_STRING_BYTES];
    char     title[GUEST_STRING_BYTES];
};

} // namespace

static_assert(sizeof(guest_system_info) == 792, "NVIDIA's layout of the guest system information");
static_assert(sizeof(FIRMWARE_VERSION) <= GUEST_STRING_BYTES, "the driver's version fits its room");

// The bytes of the reply's copy of the arguments that give the VGX version, the first two fields
constexpr uint32_t VGX_VERSION_BYTES = 2 * sizeof(uint32_t);

int32_t confirm_message_version(gsp& conversation, uint64_t timeout_ns) {
    guest_system_info info = {};
    info.vgx_version_major = VGX_VERSION_MAJOR;
    info.vgx_version_minor = VGX_VERSION_MINOR;
    info.driver_version_bytes = GUEST_STRING_BYTES;
    info.branch_bytes = GUEST_STRING_BYTES;
    info.title_bytes = GUEST_STRING_BYTES;
    string::memcpy(info.driver_version, FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION));

    received_message reply = {};
    int32_t rc = conversation.request(
        FUNCTION_SET_GUEST_SYSTEM_INFO,
        &info,
        sizeof(info),
        sizeof(info),
        reply,
        timeout_ns
    );
    if (rc != OK) {
        return rc;
    }

    if (gsp::to_error(reply.private_result) != OK) {
        log::error(
            "nvidia: the GSP's firmware refuses VGX version 0x%X.%02X for the driver's messages, with private result "
            "0x%X, in a reply of %u payload bytes, after which the version reads 0x%X.%02X",
            VGX_VERSION_MAJOR,
            VGX_VERSION_MINOR,
            reply.private_result,
            reply.payload_bytes,
            info.vgx_version_major,
            info.vgx_version_minor
        );
        return ERR_FIRMWARE;
    }

    if (reply.payload_bytes < VGX_VERSION_BYTES) {
        log::error(
            "nvidia: the GSP's firmware replies to the driver's VGX version with %u payload bytes, too few to give the "
            "version it accepts",
            reply.payload_bytes
        );
        return ERR_BAD_MESSAGE;
    }

    bool is_same_version = info.vgx_version_major == VGX_VERSION_MAJOR && info.vgx_version_minor == VGX_VERSION_MINOR;
    if (!is_same_version) {
        log::error(
            "nvidia: the GSP's firmware replies with VGX version 0x%X.%02X, while the driver's messages follow "
            "0x%X.%02X",
            info.vgx_version_major,
            info.vgx_version_minor,
            VGX_VERSION_MAJOR,
            VGX_VERSION_MINOR
        );
        return ERR_FIRMWARE;
    }

    return OK;
}

} // namespace drivers::nvidia
