#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_GPU_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_GPU_H

#include "common/types.h"

namespace pci { class device; }

// NVIDIA RTX 3080 (GA102) vertical bring-up driver — x86_64 only.
//
// This is deliberately NOT a general GPU driver: it is the minimal host-side
// code path to cold-boot one specific RTX 3080 and eventually light one pixel,
// reusing NVIDIA's GSP firmware verbatim. See gpu-driver-reverse-engineering-spec.md.
namespace nvidia {

constexpr int32_t OK            = 0;
constexpr int32_t ERR_NO_DEVICE = -1; // RTX 3080 not present in PCI enumeration
constexpr int32_t ERR_NO_BAR    = -2; // BAR0 missing / zero-sized
constexpr int32_t ERR_MAP       = -3; // BAR0 ioremap failed
constexpr int32_t ERR_CHIP_LOST = -4; // BAR0 reads 0xFFFFFFFF (GPU off the bus)
constexpr int32_t ERR_GFW       = -5; // GPU-firmware devinit did not complete

/**
 * @brief Full GA102 bring-up (GSP boot -> display -> modeset -> cursor). Runs on the pci_driver
 * task (ring 3); the framework passes the bound pci::device*. Enables Memory + Bus-Master, maps
 * BAR0 (PAGE_USER), boots the GSP, and brings up the display. Logs verbosely over serial.
 * @param dev the GA102 device the pci_driver framework bound (nv_driver.cpp).
 * @return OK on success; negative error code otherwise.
 */
int32_t init(pci::device* dev);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_GPU_H
