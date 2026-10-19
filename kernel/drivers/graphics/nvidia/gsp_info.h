// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_INFO_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_INFO_H

#include "common/types.h"
#include "drivers/graphics/nvidia/gsp.h"

namespace drivers::nvidia {

// NVIDIA's number for the first request its driver sends once the GSP's firmware is ready, which the reply carries too
constexpr uint32_t FUNCTION_SET_GUEST_SYSTEM_INFO = 0x01;

// The VGX version (NVIDIA's number for the version of the messages between its driver and the firmware) that every
// message this driver builds follows, as NVIDIA's driver builds them for this firmware
constexpr uint32_t VGX_VERSION_MAJOR = 0x23;
constexpr uint32_t VGX_VERSION_MINOR = 0x05;

/**
 * @brief Tell the GSP's firmware which version of NVIDIA's messages this
 * driver builds, VGX version 0x23.05, and check that the firmware accepts it,
 * as NVIDIA's driver does with the first request it sends once the firmware is
 * ready. The request goes through `gsp::request` and carries the version, the
 * room of 256 bytes NVIDIA gives each of the three strings that follow and, as
 * the driver's version, the firmware's version string, which equals the one
 * NVIDIA's driver sends as its own. NVIDIA's branch name, title and changelist
 * number name its own build, so they are sent empty and 0. The firmware
 * accepts the version when the reply's private result is NVIDIA's success
 * value and the reply's copy of the arguments gives the version sent. NVIDIA's
 * driver reads the version whatever the reply's length, while here the payload
 * must hold both version fields, since the arguments past a shorter payload
 * keep the version sent. NVIDIA's driver also accepts another version a
 * successful reply gives, building its later messages for that version, and
 * sends the request again at the version a refusing reply gives when that
 * version differs from the one sent and is at least 0x18.00, while this driver
 * builds its messages for 0x23.05 alone, so either reply fails the call. The
 * call spins on the calling CPU without sleeping, through the wait for the
 * reply, which `gsp::request` bounds.
 * @param conversation The driver's `gsp`, whose status queue must be linked.
 * @param timeout_ns How long the wait for the reply may take, as
 * `gsp::request` takes it, `RECEIVE_TIMEOUT_NS` at bring-up.
 * @return `OK` once the firmware accepts the version, what `gsp::request`
 * returns when the request fails, logged as its doc describes, `ERR_FIRMWARE`
 * when the reply refuses the version or gives another, or `ERR_BAD_MESSAGE`
 * when a reply that does not refuse it is too short to give the version, both
 * logged.
 */
int32_t confirm_message_version(gsp& conversation, uint64_t timeout_ns);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_INFO_H
