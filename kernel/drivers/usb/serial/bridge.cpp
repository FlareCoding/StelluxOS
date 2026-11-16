#include "drivers/usb/serial/bridge.h"
#include "drivers/usb/core/usb_identity.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

namespace usb::serial {

// Many small reads rather than a few large ones, since each read ends at the chip's next short packet, and enough
// must stay posted to cover a delay before the HCD task posts them again
static constexpr uint32_t READ_BYTES = 256;
static constexpr uint32_t POSTED_READS = 32;

static const interface* find_interface(const device& dev, uint8_t number) {
    for (uint8_t i = 0; i < dev.num_interfaces; i++) {
        if (dev.interfaces[i].interface_number == number) {
            return &dev.interfaces[i];
        }
    }

    return nullptr;
}

// A read holds whole packets, since a full packet past its end halts the endpoint
static uint32_t round_up_to_packets(uint32_t bytes, uint16_t packet_size) {
    return (bytes + packet_size - 1) / packet_size * packet_size;
}

bridge::bridge(translator* chip_translator) : class_driver("usb-serial"), m_translator(chip_translator) {
    m_disconnect_wq.init();
}

// The streams live only inside run(), so a bridge the USB core frees on the HCD task never waits for one to close
bridge::~bridge() {
    heap::ufree_delete(m_translator);
}

int32_t bridge::probe(device* dev, interface* iface) {
    int32_t rc = m_translator->configure();
    if (rc == ::serial::ERR_UNSUPPORTED) {
        log::info("usb-serial: %04x:%04x interface %u is not one its translator drives", dev->vid, dev->pid,
                  iface->interface_number);
        return -1;
    }

    if (rc != ::serial::OK) {
        log::error("usb-serial: %04x:%04x interface %u did not answer its setup requests: %d", dev->vid, dev->pid,
                   iface->interface_number, rc);
        return -1;
    }

    const interface* data_interface = find_interface(*dev, m_translator->data_interface_number());
    const endpoint* bulk_in = data_interface ? find_bulk_in(*data_interface) : nullptr;
    const endpoint* bulk_out = data_interface ? find_bulk_out(*data_interface) : nullptr;
    if (!bulk_in) {
        log::warn("usb-serial: %04x:%04x has no bulk IN endpoint on interface %u", dev->vid, dev->pid,
                  m_translator->data_interface_number());
        return -1;
    }

    if (!bulk_out) {
        log::warn("usb-serial: %04x:%04x has no bulk OUT endpoint on interface %u", dev->vid, dev->pid,
                  m_translator->data_interface_number());
        return -1;
    }

    m_bulk_in_address = bulk_in->address;
    m_bulk_out_address = bulk_out->address;
    m_packet_size = bulk_in->max_packet_size;
    return 0;
}

void bridge::run() {
    // Registered here rather than in probe(), since the USB core can free a driver after its probe without running it
    ::serial::port* registered = register_serial_port();

    bool is_published = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
        if (!m_is_gone) {
            m_port.store_release(registered);
            is_published = registered != nullptr;
        } else if (registered) {
            // A disconnect() during the registration found no port to detach
            ::serial::detach(registered);
        }

        sync::spin_unlock_irqrestore(m_lock, irq);
    });

    // Reads start only once the port is published, so a read that fails always has a port to report it to
    device* dev = bound_device();
    uint32_t read_length = round_up_to_packets(READ_BYTES, m_packet_size);
    bool is_streaming = is_published &&
                        open_in_stream(dev, m_bulk_in_address, read_length, POSTED_READS, handle_read, this,
                                       &m_stream) == 0;
    if (is_published && !is_streaming) {
        log::warn("usb-serial: %04x:%04x could not open a stream on EP 0x%02x", dev->vid, dev->pid,
                  m_bulk_in_address);
        ::serial::receive_failed(registered);
    }

    if (is_published) {
        m_translator->open_streams();
    }

    // No send starts once the device has left, and those in flight end with it
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
        while (!m_is_gone || m_sends_in_flight > 0) {
            irq = sync::wait_uninterruptible(m_disconnect_wq, m_lock, irq);
        }

        sync::spin_unlock_irqrestore(m_lock, irq);
    });

    // The stream's last call has returned once the close does, so no call reaches the port after it is unregistered
    close_in_stream(m_stream);
    m_stream = nullptr;
    m_translator->close_streams();

    if (registered) {
        ::serial::unregister_port(registered);
    }
}

void bridge::disconnect() {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
        m_is_gone = true;

        // Detached under the lock, so run() cannot unregister and free the port before it has left /dev/serial
        ::serial::port* registered = m_port.load_relaxed();
        if (registered) {
            ::serial::detach(registered);
        }

        sync::wake_all(m_disconnect_wq);
        sync::spin_unlock_irqrestore(m_lock, irq);
    });
}

int32_t bridge::set_line(const ::serial::line_settings& settings) {
    return m_translator->set_line(settings);
}

int32_t bridge::set_modem_outputs(uint32_t lines) {
    return m_translator->set_modem_outputs(lines);
}

int32_t bridge::get_modem_inputs(uint32_t* lines) {
    return m_translator->get_modem_inputs(lines);
}

int32_t bridge::set_break(bool on) {
    return m_translator->set_break(on);
}

ssize_t bridge::tx_pending() {
    return m_translator->tx_pending();
}

// The port leaves `data` untouched until the send ends, and the host controller's driver copies it when the transfer
// starts, so the bridge sends it without a copy of its own
int32_t bridge::send(const uint8_t* data, size_t length) {
    if (!count_send_start()) {
        return ::serial::ERR_IO;
    }

    init_transfer_request(m_send, m_bulk_out_address, const_cast<uint8_t*>(data), static_cast<uint32_t>(length));
    set_completion_callback(m_send, handle_send_end, this);
    if (submit_transfer_async(bound_device(), m_send) != 0) {
        count_send_end();
        return ::serial::ERR_IO;
    }

    return ::serial::OK;
}

void bridge::cancel_send() {
    cancel_transfer(bound_device(), m_send);
}

// A cancel reports the bytes the controller reports taken rather than a failure, so TCOFLUSH or a close never fails
// the port's later writes
void bridge::handle_send_end(void* context, transfer_request& request) {
    auto* self = static_cast<bridge*>(context);
    bool is_sent = request.status == transfer_status::ok ||
                   request.status == transfer_status::short_packet ||
                   request.status == transfer_status::cancelled;
    ssize_t taken_or_error = is_sent ? static_cast<ssize_t>(request.actual_length) : ::serial::ERR_IO;

    ::serial::send_ended(self->m_port.load_acquire(), taken_or_error);
    self->count_send_end();
}

// A send starts only while the port is published and the device attached, so its end always reaches a port and run()
// waits for every send
bool bridge::count_send_start() {
    bool is_counted = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
        if (!m_is_gone && m_port.load_relaxed()) {
            m_sends_in_flight++;
            is_counted = true;
        }

        sync::spin_unlock_irqrestore(m_lock, irq);
    });

    return is_counted;
}

// A send's callback can still be returning as the port starts the next send, which a count allows for
void bridge::count_send_end() {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
        m_sends_in_flight--;
        if (m_sends_in_flight == 0) {
            sync::wake_all(m_disconnect_wq);
        }

        sync::spin_unlock_irqrestore(m_lock, irq);
    });
}

void bridge::handle_read(void* context, const uint8_t* data, uint32_t length, transfer_status status) {
    auto* self = static_cast<bridge*>(context);
    ::serial::port* registered = self->m_port.load_acquire();
    if (status == transfer_status::ok || status == transfer_status::short_packet) {
        self->m_translator->handle_received(registered, data, length, self->m_packet_size);
        return;
    }

    // An unplug or a close ends the stream as expected, and any other status is a read that failed
    bool is_expected_end = status == transfer_status::device_gone || status == transfer_status::cancelled;
    if (!is_expected_end) {
        ::serial::receive_failed(registered);
    }
}

::serial::port* bridge::register_serial_port() {
    device* dev = bound_device();
    char location[core::LOCATION_TEXT_BYTES];
    core::format_location(*dev, location);

    ::serial::identity id = {
        .chip = m_translator->chip_name(),
        .serial_number = dev->serial_number,
        .location = location,
        .channel = m_translator->channel(),
        .bus = "usb",
        .driver = m_translator->driver_name(),
        .vid = dev->vid,
        .pid = dev->pid,
        .manufacturer = dev->manufacturer,
        .product = dev->product,
    };

    return ::serial::register_port(this, id);
}

} // namespace usb::serial
