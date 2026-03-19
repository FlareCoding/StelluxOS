#include "drivers/usb/hub/hub_driver.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/xhci/xhci.h"
#include "drivers/usb/xhci/xhci_device.h"
#include "drivers/usb/xhci/xhci_common.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"
#include "hw/delay.h"

namespace usb::hub {

hub_driver::hub_driver(usb::device* dev, usb::interface* iface)
    : class_driver("usb-hub"), m_dev(dev), m_iface(iface) {
    string::memset(&m_hub_desc, 0, sizeof(m_hub_desc));
}

int32_t hub_driver::probe(usb::device* dev, usb::interface* /*iface*/) {
    if (get_hub_descriptor(&m_hub_desc) != 0) {
        log::error("hub: failed to get hub descriptor");
        return -1;
    }

    uint8_t num_ports = m_hub_desc.bNbrPorts;
    if (num_ports == 0 || num_ports > MAX_HUB_PORTS) {
        log::error("hub: invalid port count %u", num_ports);
        return -1;
    }

    log::info("hub: %u port(s), pwrOn2PwrGood=%u ms, characteristics=0x%04x",
              num_ports, m_hub_desc.bPwrOn2PwrGood * 2,
              m_hub_desc.wHubCharacteristics);

    // Tell the xHCI HCD this device is a hub
    auto* hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);

    // Extract TT think time from wHubCharacteristics bits 5-6
    uint8_t tt_think_time = (m_hub_desc.wHubCharacteristics >> 5) & 0x3;
    // MTT: bit 0 of wHubCharacteristics (0=single TT, 1=multi TT for ganged)
    // Actually, per USB 2.0 spec, if the hub supports multiple TTs, it's
    // indicated by bDeviceProtocol == 2 (multi-TT). We don't enable MTT for now.
    bool mtt = false;

    if (hcd->configure_as_hub(xdev, num_ports, tt_think_time, mtt) != 0) {
        log::error("hub: failed to configure xHCI slot as hub");
        return -1;
    }

    dev->is_hub = true;
    dev->hub_num_ports = num_ports;

    return 0;
}

void hub_driver::run() {
    uint8_t ep_addr = find_interrupt_in_endpoint();
    auto* hcd = static_cast<drivers::xhci_hcd*>(m_dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(m_dev->hcd_device);

    // Power on all ports
    power_on_ports();

    // Initial port scan after power-on.
    // Devices already physically connected won't have PORT_CHANGE_CONNECTION
    // set, so we enumerate based on PORT_STATUS_CONNECTION directly.
    delay::us(m_hub_desc.bPwrOn2PwrGood * 2000 + 50000);

    for (uint8_t port = 1; port <= m_hub_desc.bNbrPorts; port++) {
        hub_port_status status = {};
        if (get_port_status(port, &status) != 0) continue;

        // Clear any stale change bits from before we took ownership
        if (status.change & PORT_CHANGE_CONNECTION)
            clear_port_feature(port, PORT_FEATURE_C_CONNECTION);
        if (status.change & PORT_CHANGE_RESET)
            clear_port_feature(port, PORT_FEATURE_C_RESET);
        if (status.change & PORT_CHANGE_ENABLE)
            clear_port_feature(port, PORT_FEATURE_C_ENABLE);

        if (!(status.status & PORT_STATUS_CONNECTION)) continue;

        log::info("hub: device present on port %u at startup", port);

        if (reset_port(port) != 0) {
            log::error("hub: port %u initial reset failed", port);
            continue;
        }

        if (get_port_status(port, &status) != 0) continue;
        if (!(status.status & PORT_STATUS_ENABLE)) {
            log::warn("hub: port %u not enabled after initial reset", port);
            continue;
        }

        uint8_t speed = hub_speed_to_xhci_speed(status.status);
        hcd->queue_hub_enumerate(xdev, port, speed);
    }

    if (ep_addr == 0) {
        log::warn("hub: no interrupt IN endpoint, polling mode only");
        // No interrupt endpoint — just stay alive
        while (!m_disconnected) {
            delay::us(500000);
            for (uint8_t port = 1; port <= m_hub_desc.bNbrPorts; port++) {
                hub_port_status status = {};
                if (get_port_status(port, &status) == 0 && status.change != 0) {
                    handle_port_change(port);
                }
            }
        }
        return;
    }

    log::info("hub: starting status change monitor on EP 0x%02x", ep_addr);

    // Status change bitmap: 1 byte is enough for hubs up to 7 ports,
    // 2 bytes for up to 15 ports (bit 0 = hub status, bit N = port N)
    uint8_t sc_buf[4] = {};
    uint8_t sc_size = (m_hub_desc.bNbrPorts / 8) + 1;
    if (sc_size > sizeof(sc_buf)) sc_size = sizeof(sc_buf);

    while (!m_disconnected) {
        int32_t rc = usb::interrupt_transfer(m_dev, ep_addr, sc_buf, sc_size);
        if (rc != 0) {
            if (m_disconnected) break;
            log::warn("hub: interrupt transfer failed (%d), retrying", rc);
            delay::us(100000);
            continue;
        }

        // Check which ports have changes (bit N = port N, bit 0 = hub status)
        for (uint8_t port = 1; port <= m_hub_desc.bNbrPorts; port++) {
            uint8_t byte_idx = port / 8;
            uint8_t bit_idx = port % 8;
            if (byte_idx < sc_size && (sc_buf[byte_idx] & (1 << bit_idx))) {
                handle_port_change(port);
            }
        }
    }

    log::info("hub: driver exiting");
}

void hub_driver::disconnect() {
    m_disconnected = true;
}

void hub_driver::handle_port_change(uint8_t port) {
    hub_port_status status = {};
    if (get_port_status(port, &status) != 0) {
        log::warn("hub: failed to get status for port %u", port);
        return;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(m_dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(m_dev->hcd_device);

    // Clear all change bits first
    if (status.change & PORT_CHANGE_CONNECTION) {
        clear_port_feature(port, PORT_FEATURE_C_CONNECTION);
    }
    if (status.change & PORT_CHANGE_ENABLE) {
        clear_port_feature(port, PORT_FEATURE_C_ENABLE);
    }
    if (status.change & PORT_CHANGE_RESET) {
        clear_port_feature(port, PORT_FEATURE_C_RESET);
    }
    if (status.change & PORT_CHANGE_OVER_CURRENT) {
        clear_port_feature(port, PORT_FEATURE_C_OVER_CURRENT);
    }
    if (status.change & PORT_CHANGE_SUSPEND) {
        clear_port_feature(port, PORT_FEATURE_C_SUSPEND);
    }

    if (status.change & PORT_CHANGE_CONNECTION) {
        if (status.status & PORT_STATUS_CONNECTION) {
            log::info("hub: device connected on port %u", port);

            if (reset_port(port) != 0) {
                log::error("hub: port %u reset failed", port);
                return;
            }

            // Re-read status after reset to get speed
            if (get_port_status(port, &status) != 0) {
                log::error("hub: failed to read post-reset status for port %u", port);
                return;
            }

            if (!(status.status & PORT_STATUS_ENABLE)) {
                log::warn("hub: port %u not enabled after reset", port);
                return;
            }

            uint8_t speed = hub_speed_to_xhci_speed(status.status);
            hcd->queue_hub_enumerate(xdev, port, speed);
        } else {
            log::info("hub: device disconnected from port %u", port);
            hcd->queue_hub_disconnect(xdev, port);
        }
    }
}

int32_t hub_driver::reset_port(uint8_t port) {
    if (set_port_feature(port, PORT_FEATURE_RESET) != 0) {
        return -1;
    }

    // Wait for reset completion (PORT_CHANGE_RESET set, PORT_STATUS_RESET cleared)
    constexpr uint32_t RESET_TIMEOUT_US = 500000;
    constexpr uint32_t POLL_US = 10000;
    uint32_t elapsed = 0;

    while (elapsed < RESET_TIMEOUT_US) {
        delay::us(POLL_US);
        elapsed += POLL_US;

        hub_port_status status = {};
        if (get_port_status(port, &status) != 0) {
            return -1;
        }

        if (status.change & PORT_CHANGE_RESET) {
            clear_port_feature(port, PORT_FEATURE_C_RESET);
            // Additional settling time
            delay::us(10000);
            return 0;
        }
    }

    log::warn("hub: port %u reset timed out", port);
    return -1;
}

void hub_driver::power_on_ports() {
    for (uint8_t port = 1; port <= m_hub_desc.bNbrPorts; port++) {
        set_port_feature(port, PORT_FEATURE_POWER);
    }
}

int32_t hub_driver::get_hub_descriptor(hub_descriptor* out) {
    return usb::control_transfer(m_dev,
        HUB_REQTYPE_GET_HUB,
        HUB_REQUEST_GET_DESCRIPTOR,
        (USB_DESCRIPTOR_HUB << 8) | 0,
        0,
        out, sizeof(hub_descriptor));
}

int32_t hub_driver::get_port_status(uint8_t port, hub_port_status* out) {
    return usb::control_transfer(m_dev,
        HUB_REQTYPE_GET_PORT,
        HUB_REQUEST_GET_STATUS,
        0,
        port,
        out, sizeof(hub_port_status));
}

int32_t hub_driver::set_port_feature(uint8_t port, uint16_t feature) {
    return usb::control_transfer(m_dev,
        HUB_REQTYPE_SET_PORT,
        HUB_REQUEST_SET_FEATURE,
        feature,
        port,
        nullptr, 0);
}

int32_t hub_driver::clear_port_feature(uint8_t port, uint16_t feature) {
    return usb::control_transfer(m_dev,
        HUB_REQTYPE_SET_PORT,
        HUB_REQUEST_CLEAR_FEATURE,
        feature,
        port,
        nullptr, 0);
}

uint8_t hub_driver::find_interrupt_in_endpoint() const {
    for (uint8_t i = 0; i < m_iface->num_endpoints; i++) {
        auto& ep = m_iface->endpoints[i];
        if (ep.transfer_type == 3 && ep.is_in()) {
            return ep.address;
        }
    }
    return 0;
}

uint8_t hub_driver::hub_speed_to_xhci_speed(uint16_t port_status) {
    if (port_status & PORT_STATUS_HIGH_SPEED) {
        return XHCI_USB_SPEED_HIGH_SPEED;
    }
    if (port_status & PORT_STATUS_LOW_SPEED) {
        return XHCI_USB_SPEED_LOW_SPEED;
    }
    return XHCI_USB_SPEED_FULL_SPEED;
}

} // namespace usb::hub

REGISTER_USB_CLASS_DRIVER(hub_driver,
    USB_MATCH(usb::USB_CLASS_HUB, usb::USB_MATCH_ANY, usb::USB_MATCH_ANY),
    USB_CLASS_DRIVER_FACTORY(usb::hub::hub_driver));
