#ifndef STELLUX_DRIVERS_USB_SERIAL_TRANSLATOR_H
#define STELLUX_DRIVERS_USB_SERIAL_TRANSLATOR_H

#include "drivers/usb/core/usb_device.h"
#include "drivers/serial/serial.h"

namespace usb::serial {

/**
 * A translator holds the code for one family of USB bridge chips or one USB
 * class, such as CDC-ACM. The bridge driver bound to the device's interface
 * does what every device shares, and asks the translator for what differs.
 */
class translator {
public:
    translator(device* dev, const interface& iface) : m_device(dev), m_interface(&iface) {}
    virtual ~translator() = default;

    translator(const translator&) = delete;
    translator& operator=(const translator&) = delete;

    /**
     * @brief Sets the chip up to receive, on the HCD task while the bridge
     * driver probes and before it looks for the bulk endpoints. Each request
     * holds the HCD task, and so every device on the controller, until it ends,
     * so it sends only what must come before reads are posted.
     * @return ::serial::OK, ::serial::ERR_IO when the chip does not answer, or
     *         ::serial::ERR_UNSUPPORTED for a chip the translator cannot drive.
     *         Either error leaves the interface unbound.
     */
    virtual int32_t configure() = 0;

    // The chip's short name, which starts its port's name, such as "ft232bm", known once `configure` succeeds
    virtual const char* chip_name() const = 0;

    // The translator's name, which the listing shows as the port's driver, such as "ftdi"
    virtual const char* driver_name() const = 0;

    // The port's number on its device, as `::serial::identity::channel` counts it
    virtual int8_t channel() const { return -1; }

    // The interface whose bulk pair carries the port's bytes, known once `configure` succeeds
    virtual uint8_t data_interface_number() const { return m_interface->interface_number; }

    // Opens the translator's own streams, such as CDC-ACM's notifications, on the bridge driver's task once the port
    // is published, so a bridge freed on the HCD task never has one to close. The default opens none
    virtual void open_streams() {}

    // Closes what `open_streams` opened, on the bridge driver's task once the device has left
    virtual void close_streams() {}

    /**
     * @brief Passes one completed read's bytes to `p`, less what the chip adds
     * to them. `data` holds whole packets of `packet_size` bytes, the bulk IN
     * endpoint's, each starting at a multiple of it, and only the last may be
     * short. The default passes the bytes unchanged. Runs on the HCD task, so
     * it must not block.
     */
    virtual void handle_received(::serial::port* p, const uint8_t* data, size_t length, uint16_t packet_size);

    // Do what `::serial::port_driver`'s calls of the same names ask of the chip
    virtual int32_t set_line(const ::serial::line_settings& settings) = 0;
    virtual int32_t set_modem_outputs(uint32_t lines) = 0;
    virtual int32_t get_modem_inputs(uint32_t* lines) = 0;
    virtual int32_t set_break(bool on) = 0;
    virtual ssize_t tx_pending() = 0;

protected:
    device*          m_device;
    const interface* m_interface;
};

// The first bulk IN endpoint of `iface` with a packet size, or null when it has none
const endpoint* find_bulk_in(const interface& iface);

// The first bulk OUT endpoint of `iface` with a packet size, or null when it has none
const endpoint* find_bulk_out(const interface& iface);

// The first interrupt IN endpoint of `iface` with a packet size, or null when it has none
const endpoint* find_interrupt_in(const interface& iface);

// The TIOCM bits of the CTS, DSR, RI and CD that several chips report in bits 4 to 7 of a modem status byte
uint32_t modem_inputs_from_status(uint8_t status);

// Stores `value` in the 4 bytes at `bytes`, least significant first, as USB requests carry it
void store_le32(uint8_t* bytes, uint32_t value);

} // namespace usb::serial

#endif // STELLUX_DRIVERS_USB_SERIAL_TRANSLATOR_H
