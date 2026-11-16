#ifndef STELLUX_DRIVERS_USB_SERIAL_BRIDGE_H
#define STELLUX_DRIVERS_USB_SERIAL_BRIDGE_H

#include "drivers/usb/core/usb_driver.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/serial/translator.h"
#include "mm/heap.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace usb::serial {

/**
 * Turns one interface of a USB bridge chip, which carries a board's UART over
 * USB, into a serial port, with a translator for the chip, which it owns.
 * Reads stay posted on the bulk IN endpoint from the port's registration to
 * the unplug, since a chip left with none posted can resend its last packet.
 * The bridge is also the port's driver, and passes the port's settings to the
 * translator.
 */
class bridge final : public class_driver, public ::serial::port_driver {
public:
    explicit bridge(translator* chip_translator);
    ~bridge() override;

    int32_t probe(device* dev, interface* iface) override;
    void run() override;
    void disconnect() override;

    int32_t set_line(const ::serial::line_settings& settings) override;

private:
    static void handle_read(void* context, const uint8_t* data, uint32_t length, transfer_status status);

    ::serial::port* register_serial_port();

    translator* m_translator;
    uint8_t     m_bulk_in_address = 0;
    uint16_t    m_packet_size = 0;
    in_stream*  m_stream = nullptr;

    // The lock guards the port pointer and the gone flag, and stream callbacks read the pointer with acquire ordering
    sync::spinlock                m_lock = sync::SPINLOCK_INIT;
    sync::wait_queue              m_disconnect_wq;
    sync::atomic<::serial::port*> m_port;
    bool                          m_is_gone = false;
};

/**
 * @brief Creates a bridge driver with a `Translator` for the interface, as the
 * USB core's factory for the chip's match entries.
 */
template <typename Translator>
class_driver* create_bridge(device* dev, interface* iface) {
    auto* chip_translator = heap::ualloc_new<Translator>(dev, *iface);
    if (!chip_translator) {
        return nullptr;
    }

    auto* driver = heap::ualloc_new<bridge>(chip_translator);
    if (!driver) {
        heap::ufree_delete(chip_translator);
    }

    return driver;
}

} // namespace usb::serial

#endif // STELLUX_DRIVERS_USB_SERIAL_BRIDGE_H
