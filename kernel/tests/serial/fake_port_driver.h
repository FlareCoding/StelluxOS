#ifndef STELLUX_TESTS_SERIAL_FAKE_PORT_DRIVER_H
#define STELLUX_TESTS_SERIAL_FAKE_PORT_DRIVER_H

#include "helpers.h"
#include "drivers/serial/serial.h"
#include "common/string.h"
#include "sync/atomic.h"

namespace serial_test {

// A port driver whose chip answers at once, recording what the port asks of it and failing a call when told to.
// Sends stay in flight until the test ends them, and a call the test gates waits inside the chip until it opens
class fake_port_driver final : public serial::port_driver {
public:
    int32_t set_line(const serial::line_settings& settings) override {
        is_in_set_line.store_release(1);
        while (set_line_gate.load_acquire()) {
            test_helpers::brief_delay();
        }

        set_line_calls++;
        if (set_line_result == serial::OK) {
            line = settings;
        }

        is_in_set_line.store_release(0);
        return set_line_result;
    }

    int32_t send(const uint8_t* data, size_t length) override {
        is_in_send.store_release(1);
        while (send_gate.load_acquire()) {
            test_helpers::brief_delay();
        }

        sent_length = length;
        string::memcpy(sent, data, length);
        is_in_send.store_release(0);
        if (send_result != serial::OK) {
            return send_result;
        }

        is_sending = true;
        started_sends.fetch_add_relaxed(1);
        if (ends_sends_at_once) {
            end_send(static_cast<ssize_t>(length));
        }

        return serial::OK;
    }

    // Ends the send in flight as a cancelled transfer does, with none of its bytes taken. A held cancel returns only
    // once the test opens its gate, after the end is reported, as a controller's cancel returns after its callback
    void cancel_send() override {
        cancels++;
        if (is_sending) {
            end_send(0);
        }

        is_in_cancel.store_release(1);
        while (cancel_gate.load_acquire()) {
            test_helpers::brief_delay();
        }

        is_in_cancel.store_release(0);
    }

    // Reports the end of the send in flight, as the controller's completion does
    void end_send(ssize_t taken_or_error) {
        is_sending = false;
        serial::send_ended(port, taken_or_error);
    }

    void reset() {
        port = nullptr;
        line = {};
        set_line_calls = 0;
        set_line_result = serial::OK;
        set_line_gate.store_release(0);
        is_in_set_line.store_release(0);

        started_sends.store_relaxed(0);
        sent_length = 0;
        send_result = serial::OK;
        ends_sends_at_once = false;
        is_sending = false;
        cancels = 0;

        send_gate.store_release(0);
        is_in_send.store_release(0);
        cancel_gate.store_release(0);
        is_in_cancel.store_release(0);
    }

    serial::port*          port = nullptr; // the port it drives, set once the test registers it
    serial::line_settings  line = {};      // what the chip was last set to
    uint32_t               set_line_calls = 0;
    int32_t                set_line_result = serial::OK;
    sync::atomic<uint32_t> set_line_gate{0};
    sync::atomic<uint32_t> is_in_set_line{0};

    sync::atomic<uint32_t> started_sends{0};
    uint8_t                sent[serial::SEND_BYTES] = {};
    size_t                 sent_length = 0;
    int32_t                send_result = serial::OK;
    bool                   ends_sends_at_once = false;
    bool                   is_sending = false;
    uint32_t               cancels = 0;
    sync::atomic<uint32_t> send_gate{0};
    sync::atomic<uint32_t> is_in_send{0};
    sync::atomic<uint32_t> cancel_gate{0};
    sync::atomic<uint32_t> is_in_cancel{0};
};

} // namespace serial_test

#endif // STELLUX_TESTS_SERIAL_FAKE_PORT_DRIVER_H
