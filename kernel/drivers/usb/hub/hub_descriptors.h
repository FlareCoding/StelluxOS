#ifndef STELLUX_DRIVERS_USB_HUB_HUB_DESCRIPTORS_H
#define STELLUX_DRIVERS_USB_HUB_HUB_DESCRIPTORS_H

#include "common/types.h"

namespace usb::hub {

// Hub Class Request Codes (USB 2.0 Spec Section 11.24.2)
constexpr uint8_t HUB_REQUEST_GET_STATUS      = 0x00;
constexpr uint8_t HUB_REQUEST_CLEAR_FEATURE   = 0x01;
constexpr uint8_t HUB_REQUEST_SET_FEATURE     = 0x03;
constexpr uint8_t HUB_REQUEST_GET_DESCRIPTOR  = 0x06;

// Hub Class Feature Selectors (USB 2.0 Spec Section 11.24.2)
constexpr uint16_t HUB_FEATURE_C_HUB_LOCAL_POWER    = 0;
constexpr uint16_t HUB_FEATURE_C_HUB_OVER_CURRENT   = 1;

// Port Feature Selectors (USB 2.0 Spec Section 11.24.2.7)
constexpr uint16_t PORT_FEATURE_CONNECTION      = 0;
constexpr uint16_t PORT_FEATURE_ENABLE          = 1;
constexpr uint16_t PORT_FEATURE_SUSPEND         = 2;
constexpr uint16_t PORT_FEATURE_OVER_CURRENT    = 3;
constexpr uint16_t PORT_FEATURE_RESET           = 4;
constexpr uint16_t PORT_FEATURE_POWER           = 8;
constexpr uint16_t PORT_FEATURE_LOW_SPEED       = 9;
constexpr uint16_t PORT_FEATURE_C_CONNECTION    = 16;
constexpr uint16_t PORT_FEATURE_C_ENABLE        = 17;
constexpr uint16_t PORT_FEATURE_C_SUSPEND       = 18;
constexpr uint16_t PORT_FEATURE_C_OVER_CURRENT  = 19;
constexpr uint16_t PORT_FEATURE_C_RESET         = 20;

// bmRequestType values for hub class requests
constexpr uint8_t HUB_REQTYPE_GET_HUB    = 0xA0; // Device-to-host, Class, Device
constexpr uint8_t HUB_REQTYPE_SET_HUB    = 0x20; // Host-to-device, Class, Device
constexpr uint8_t HUB_REQTYPE_GET_PORT   = 0xA3; // Device-to-host, Class, Other (port)
constexpr uint8_t HUB_REQTYPE_SET_PORT   = 0x23; // Host-to-device, Class, Other (port)

// wPortStatus bit definitions (USB 2.0 Spec Section 11.24.2.7.1)
constexpr uint16_t PORT_STATUS_CONNECTION    = (1 << 0);
constexpr uint16_t PORT_STATUS_ENABLE        = (1 << 1);
constexpr uint16_t PORT_STATUS_SUSPEND       = (1 << 2);
constexpr uint16_t PORT_STATUS_OVER_CURRENT  = (1 << 3);
constexpr uint16_t PORT_STATUS_RESET         = (1 << 4);
constexpr uint16_t PORT_STATUS_POWER         = (1 << 8);
constexpr uint16_t PORT_STATUS_LOW_SPEED     = (1 << 9);
constexpr uint16_t PORT_STATUS_HIGH_SPEED    = (1 << 10);

// wPortChange bit definitions (USB 2.0 Spec Section 11.24.2.7.2)
constexpr uint16_t PORT_CHANGE_CONNECTION    = (1 << 0);
constexpr uint16_t PORT_CHANGE_ENABLE        = (1 << 1);
constexpr uint16_t PORT_CHANGE_SUSPEND       = (1 << 2);
constexpr uint16_t PORT_CHANGE_OVER_CURRENT  = (1 << 3);
constexpr uint16_t PORT_CHANGE_RESET         = (1 << 4);

// Hub Descriptor (USB 2.0 Spec Section 11.23.2.1)
struct hub_descriptor {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;   // 0x29 for hub
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;    // time in 2ms intervals
    uint8_t  bHubContrCurrent;  // max current in mA
    uint8_t  device_removable[8]; // variable length bitmap
} __attribute__((packed));

// Hub port status (returned by GET_STATUS for a port)
struct hub_port_status {
    uint16_t status;
    uint16_t change;
} __attribute__((packed));
static_assert(sizeof(hub_port_status) == 4);

// Speed detection from port status bits
inline uint8_t port_speed_from_status(uint16_t status) {
    if (status & PORT_STATUS_HIGH_SPEED) return 3; // High Speed
    if (status & PORT_STATUS_LOW_SPEED)  return 2; // Low Speed
    return 1; // Full Speed
}

constexpr uint8_t MAX_HUB_PORTS = 16;

} // namespace usb::hub

#endif // STELLUX_DRIVERS_USB_HUB_HUB_DESCRIPTORS_H
