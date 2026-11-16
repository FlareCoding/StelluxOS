#ifndef STELLUX_DRIVERS_USB_SERIAL_CDC_ACM_H
#define STELLUX_DRIVERS_USB_SERIAL_CDC_ACM_H

#include "drivers/usb/serial/translator.h"
#include "drivers/usb/core/usb_transfer.h"
#include "sync/atomic.h"

namespace usb::serial {

/**
 * The translator for CDC-ACM devices, USB's standard class for serial ports.
 * The bridge driver binds to a port's control interface, which takes the
 * port's requests and reports its modem inputs. The port's bytes are carried
 * by the bulk pair of the data interface that the control interface's union
 * descriptor names. The class has no CTS line, so CTS always reads as raised.
 */
class cdc_acm final : public translator {
public:
    cdc_acm(device* dev, const interface& iface);

    int32_t configure() override;
    const char* chip_name() const override { return "acm"; }
    const char* driver_name() const override { return "cdc_acm"; }
    int8_t channel() const override { return m_channel; }
    uint8_t data_interface_number() const override { return m_data_interface_number; }
    void open_streams() override;
    void close_streams() override;
    int32_t set_line(const ::serial::line_settings& settings) override;
    int32_t set_modem_outputs(uint32_t lines) override;
    int32_t get_modem_inputs(uint32_t* lines) override;
    int32_t set_break(bool on) override;
    ssize_t tx_pending() override;

private:
    // SERIAL_STATE's 8-byte header and 2 bytes of data, the longest notification the translator keeps
    static constexpr uint32_t KEPT_NOTIFICATION_BYTES = 10;

    static void deliver_packet(void* context, const uint8_t* data, uint32_t length, transfer_status status);

    // Adds a packet to the notification it continues, `is_short` when the packet ends its transfer
    void handle_packet(const uint8_t* packet, uint32_t length, bool is_short);

    // Stores the CD, DSR and RI a SERIAL_STATE notification reports, and ignores any other notification
    void handle_notification(const uint8_t* notification, uint32_t length);

    int32_t send_request(uint8_t request, uint16_t value, void* data, uint16_t length);

    int8_t                 m_channel;
    uint8_t                m_data_interface_number;
    in_stream*             m_notification_stream;
    sync::atomic<uint32_t> m_modem_inputs;

    // The notification its packets are rebuilding, as far as it fits, and the bytes of it received so far
    uint8_t  m_notification[KEPT_NOTIFICATION_BYTES];
    uint32_t m_notification_received;
};

} // namespace usb::serial

#endif // STELLUX_DRIVERS_USB_SERIAL_CDC_ACM_H
