#ifndef STELLUX_DRIVERS_USB_HID_HID_DRIVER_H
#define STELLUX_DRIVERS_USB_HID_HID_DRIVER_H

#include "drivers/usb/core/usb_driver.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/hid/hid_parser.h"
#include "drivers/usb/hid/hid_handler.h"

namespace usb::hid {

class hid_driver : public usb::class_driver {
public:
    hid_driver(usb::device* dev, usb::interface* iface);
    ~hid_driver() override;

    int32_t probe(usb::device* dev, usb::interface* iface) override;
    void run() override;
    void disconnect() override;

private:
    enum class binding_kind : uint8_t {
        keyboard,
        mouse,
    };

    struct handler_binding {
        uint8_t report_id = 0;
        binding_kind kind = binding_kind::keyboard;
        hid_handler* handler = nullptr;
    };

    usb::interface* m_iface;
    report_layout   m_layout;
    handler_binding* m_bindings = nullptr;
    uint16_t         m_binding_count = 0;
    bool            m_disconnected = false;
    uint32_t        m_payload_length = 0;
    usb::interrupt_in_stream* m_stream = nullptr;

    const usb::endpoint* find_interrupt_in_endpoint() const;
    uint32_t max_input_report_bytes(const usb::endpoint& ep) const;
    void destroy_bindings();
    int32_t create_handlers();
    void apply_idle_policy(usb::device* dev);
    void dispatch_report(uint8_t report_id, const uint8_t* data, uint32_t length);

};

} // namespace usb::hid

#endif // STELLUX_DRIVERS_USB_HID_HID_DRIVER_H
