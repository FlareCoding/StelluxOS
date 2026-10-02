#ifndef STELLUX_DRIVERS_USB_XHCI_TEGRA_XUSB_H
#define STELLUX_DRIVERS_USB_XHCI_TEGRA_XUSB_H

#include "common/types.h"

// Tegra210 XUSB host bring-up. The XUSB block is a standard xHCI controller
// whose logic runs as NVIDIA firmware on an internal Falcon microcontroller.
// This module powers and clocks the block, routes the USB2 pads, loads the
// firmware and services its mailbox; the generic xHCI driver takes over once
// the controller reports ready.
namespace tegra_xusb {

constexpr uint64_t XHCI_PHYS = 0x70090000;
constexpr size_t   XHCI_SIZE = 0x8000;
constexpr uint32_t XHCI_IRQ  = 71; // GIC SPI 39
constexpr uint32_t MBOX_IRQ  = 72; // GIC SPI 40

constexpr int32_t OK            = 0;
constexpr int32_t ERR_MAP       = -1;
constexpr int32_t ERR_FIRMWARE  = -2;
constexpr int32_t ERR_TIMEOUT   = -3;
constexpr int32_t ERR_POWER     = -4;
constexpr int32_t ERR_NO_MEM    = -5;

/**
 * @brief Bring the XUSB host from reset to "controller ready": power
 * partitions, clocks, resets, USB2 pads, IPFS/FPCI and firmware.
 * On success the xHCI registers at XHCI_PHYS are live and USBSTS.CNR is 0.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t bring_up();

/**
 * @brief xHCI start hook: tells the firmware the host driver is running
 * (MSG_ENABLED). Matches drivers::xhci_hcd::start_hook_fn.
 */
void on_controller_started(void* context);

} // namespace tegra_xusb

#endif // STELLUX_DRIVERS_USB_XHCI_TEGRA_XUSB_H
