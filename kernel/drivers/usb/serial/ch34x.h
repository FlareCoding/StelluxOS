#ifndef STELLUX_DRIVERS_USB_SERIAL_CH34X_H
#define STELLUX_DRIVERS_USB_SERIAL_CH34X_H

#include "drivers/usb/serial/translator.h"

namespace usb::serial {

/**
 * The translator for WCH's CH340 and CH341 USB bridge chips, which take their
 * speed and character format as writes of register pairs and add nothing to
 * the bytes they receive.
 */
class ch34x final : public translator {
public:
    ch34x(device* dev, const interface& iface);

    int32_t configure() override;
    const char* chip_name() const override { return m_chip_name; }
    const char* driver_name() const override;
    int32_t set_line(const ::serial::line_settings& settings) override;
    int32_t set_modem_outputs(uint32_t lines) override;
    int32_t get_modem_inputs(uint32_t* lines) override;
    int32_t set_break(bool on) override;
    ssize_t tx_pending() override;

private:
    int32_t send_request(uint8_t request, uint16_t value, uint16_t index);
    int32_t read_registers(uint16_t registers, uint8_t* values);

    const char* m_chip_name;
    bool        m_is_breaking = false;
};

} // namespace usb::serial

#endif // STELLUX_DRIVERS_USB_SERIAL_CH34X_H
