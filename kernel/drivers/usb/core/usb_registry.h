#ifndef STELLUX_DRIVERS_USB_CORE_USB_REGISTRY_H
#define STELLUX_DRIVERS_USB_CORE_USB_REGISTRY_H

#include "drivers/usb/core/usb_device.h"

// The USB core's registry of its devices on every controller, which `/dev/sysinfo/usb` lists in location order.
// A device is removed when its unplug begins, before anything can free it

// The registry's lock comes before any device's lifetime lock, and the listing's poll observers run under it

namespace usb::core {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief Sets up the registry and adds `/dev/sysinfo/usb`, which lists every
 * device a line each and wakes its pollers as a device arrives or leaves. Must
 * run once devfs is mounted and before any controller reports a device.
 * @return OK, or ERR when the node cannot be added. The registry is set up
 *         either way.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_registry();

/**
 * Adds `dev` at its place in location order and wakes the listing's pollers.
 * Its drivers must be bound first, since binding a driver wakes no poller.
 * Never blocks, and must be called holding no lifetime lock and no poll
 * observer's lock.
 */
void list_device(usb::device* dev);

/**
 * Takes `dev`, which must be listed, out of the registry and wakes the
 * listing's pollers. Must be called before anything lets the device be freed.
 * Never blocks, and must be called holding no lifetime lock and no poll
 * observer's lock.
 */
void unlist_device(usb::device* dev);

} // namespace usb::core

#endif // STELLUX_DRIVERS_USB_CORE_USB_REGISTRY_H
