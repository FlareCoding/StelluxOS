#ifndef STELLUX_DRIVERS_USB_SERIAL_FTDI_H
#define STELLUX_DRIVERS_USB_SERIAL_FTDI_H

#include "drivers/usb/serial/translator.h"

namespace usb::serial {

struct ftdi_chip;

/**
 * The translator for FTDI's USB bridge chips.
 */
class ftdi final : public translator {
public:
    ftdi(device* dev, const interface& iface);

    int32_t configure() override;
    const char* chip_name() const override;
    const char* driver_name() const override;
    int8_t channel() const override;
    void handle_received(::serial::port* p, const uint8_t* data, size_t length, uint16_t packet_size) override;
    int32_t set_line(const ::serial::line_settings& settings) override;

private:
    void ask_for_latency_timer();
    uint16_t uart_number() const;
    int32_t send_request(uint8_t request, uint16_t value, uint16_t index);

    const ftdi_chip* m_chip;
    bool             m_has_asked_for_latency_timer = false;
};

} // namespace usb::serial

#endif // STELLUX_DRIVERS_USB_SERIAL_FTDI_H
