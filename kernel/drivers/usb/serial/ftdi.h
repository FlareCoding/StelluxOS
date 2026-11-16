#ifndef STELLUX_DRIVERS_USB_SERIAL_FTDI_H
#define STELLUX_DRIVERS_USB_SERIAL_FTDI_H

#include "drivers/usb/serial/translator.h"
#include "sync/atomic.h"

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
    int32_t set_modem_outputs(uint32_t lines) override;
    int32_t get_modem_inputs(uint32_t* lines) override;

private:
    void ask_for_latency_timer();
    uint16_t uart_number() const;
    int32_t send_request(uint8_t request, uint16_t value, uint16_t index);

    const ftdi_chip*       m_chip;
    bool                   m_has_asked_for_latency_timer = false;
    sync::atomic<uint32_t> m_modem_status; // the latest packet's modem status byte
};

} // namespace usb::serial

#endif // STELLUX_DRIVERS_USB_SERIAL_FTDI_H
