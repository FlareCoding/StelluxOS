#ifndef STELLUX_DRIVERS_USB_SERIAL_FTDI_H
#define STELLUX_DRIVERS_USB_SERIAL_FTDI_H

#include "drivers/usb/serial/translator.h"

namespace usb::serial {

/**
 * The translator for FTDI's USB bridge chips.
 */
class ftdi final : public translator {
public:
    ftdi(device* dev, const interface& iface);

    int32_t configure() override;
    const char* chip_name() const override { return m_chip_name; }
    int8_t channel() const override;
    void handle_received(::serial::port* p, const uint8_t* data, size_t length, uint16_t packet_size) override;

private:
    int32_t send_request(uint8_t request, uint16_t value);

    const char* m_chip_name;
};

} // namespace usb::serial

#endif // STELLUX_DRIVERS_USB_SERIAL_FTDI_H
