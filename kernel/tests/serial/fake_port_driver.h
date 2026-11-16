#ifndef STELLUX_TESTS_SERIAL_FAKE_PORT_DRIVER_H
#define STELLUX_TESTS_SERIAL_FAKE_PORT_DRIVER_H

#include "helpers.h"
#include "drivers/serial/serial.h"
#include "sync/atomic.h"

namespace serial_test {

// A port driver whose chip answers at once, recording what the port asks of it and failing a call when told to.
// While a test holds `set_line_gate`, set_line waits inside the chip, as a slow chip request does
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

    void reset() {
        line = {};
        set_line_calls = 0;
        set_line_result = serial::OK;
        set_line_gate.store_release(0);
        is_in_set_line.store_release(0);
    }

    serial::line_settings  line = {};      // what the chip was last set to
    uint32_t               set_line_calls = 0;
    int32_t                set_line_result = serial::OK;
    sync::atomic<uint32_t> set_line_gate{0};
    sync::atomic<uint32_t> is_in_set_line{0};
};

} // namespace serial_test

#endif // STELLUX_TESTS_SERIAL_FAKE_PORT_DRIVER_H
