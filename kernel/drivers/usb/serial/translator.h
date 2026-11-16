#ifndef STELLUX_DRIVERS_USB_SERIAL_TRANSLATOR_H
#define STELLUX_DRIVERS_USB_SERIAL_TRANSLATOR_H

#include "drivers/usb/core/usb_device.h"
#include "drivers/serial/serial.h"

namespace usb::serial {

/**
 * A translator holds the code for one family of USB bridge chips. The bridge
 * driver bound to the chip's interface does what every chip shares, and asks
 * the translator for what differs.
 */
class translator {
public:
    translator(device* dev, const interface& iface) : m_device(dev), m_interface(&iface) {}
    virtual ~translator() = default;

    translator(const translator&) = delete;
    translator& operator=(const translator&) = delete;

    /**
     * @brief Sets the chip up to receive, on the HCD task while the bridge
     * driver probes and before it looks for the bulk IN endpoint. Each request
     * holds the HCD task, and so every device on the controller, until it ends,
     * so it sends only what must come before reads are posted.
     * @return ::serial::OK, ::serial::ERR_IO when the chip does not answer, or
     *         ::serial::ERR_UNSUPPORTED for a chip the translator cannot drive.
     *         Either error leaves the interface unbound.
     */
    virtual int32_t configure() = 0;

    // The chip's short name, which starts its port's name, such as "ft232bm", known once `configure` succeeds
    virtual const char* chip_name() const = 0;

    // The port's number on its device, as `::serial::identity::channel` counts it
    virtual int8_t channel() const { return -1; }

    /**
     * @brief Passes one completed read's bytes to `p`, less what the chip adds
     * to them. `data` holds whole packets of `packet_size` bytes, the bulk IN
     * endpoint's, each starting at a multiple of it, and only the last may be
     * short. The default passes the bytes unchanged. Runs on the HCD task, so
     * it must not block.
     */
    virtual void handle_received(::serial::port* p, const uint8_t* data, size_t length, uint16_t packet_size);

protected:
    device*          m_device;
    const interface* m_interface;
};

// The first bulk IN endpoint of `iface` with a packet size, or null when it has none
const endpoint* find_bulk_in(const interface& iface);

} // namespace usb::serial

#endif // STELLUX_DRIVERS_USB_SERIAL_TRANSLATOR_H
