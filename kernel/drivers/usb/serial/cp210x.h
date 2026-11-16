#ifndef STELLUX_DRIVERS_USB_SERIAL_CP210X_H
#define STELLUX_DRIVERS_USB_SERIAL_CP210X_H

#include "drivers/usb/serial/translator.h"

namespace usb::serial {

struct cp210x_part;

/**
 * The translator for Silicon Labs' CP210x USB bridge chips, whose requests
 * each name the interface of the UART they concern.
 */
class cp210x final : public translator {
public:
    cp210x(device* dev, const interface& iface);

    int32_t configure() override;
    const char* chip_name() const override;
    const char* driver_name() const override;
    int32_t set_line(const ::serial::line_settings& settings) override;
    int32_t set_modem_outputs(uint32_t lines) override;
    int32_t get_modem_inputs(uint32_t* lines) override;
    int32_t set_break(bool on) override;
    ssize_t tx_pending() override;

private:
    int32_t send_request(uint8_t request, uint16_t value, void* data, uint16_t length);
    int32_t read_request(uint8_t request, uint16_t value, void* reply, uint16_t length);
    int32_t send_flow_control(bool uses_rts_cts);

    const cp210x_part* m_part;
    uint32_t           m_modem_outputs = 0; // DTR and RTS as last set, which SET_FLOW sets again
    bool               m_uses_rts_cts = false;
};

} // namespace usb::serial

#endif // STELLUX_DRIVERS_USB_SERIAL_CP210X_H
