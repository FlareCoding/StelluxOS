#ifndef STELLUX_DRIVERS_USB_HID_HID_PARSER_H
#define STELLUX_DRIVERS_USB_HID_HID_PARSER_H

#include "drivers/usb/hid/hid_constants.h"

namespace usb::hid {

// A single decoded item from a HID report descriptor
struct report_item {
    item_type type;
    uint8_t   tag;
    uint8_t   size;  // data payload size in bytes (0, 1, 2, or 4)
    uint32_t  data;  // data payload (little-endian, zero-extended)
};

// A parsed field within the input report — describes where to find
// a value (buttons, axis, wheel, etc.) in the raw interrupt data
struct field_info {
    uint16_t bit_offset;
    uint8_t  bit_size;
    uint16_t usage_page;
    uint16_t usage;
};

// Parsed report layout — the result of parsing a HID report descriptor.
// Owns heap-allocated arrays; call destroy() when done.
struct report_layout {
    field_info* fields = nullptr;
    uint16_t    num_fields = 0;

    void destroy();
};

// Parse a raw HID report descriptor into a report_layout.
// The layout's fields array is heap-allocated; caller must call layout.destroy().
// Returns 0 on success, negative on failure.
int32_t parse_report_descriptor(const uint8_t* descriptor, size_t length,
                                report_layout& out);

} // namespace usb::hid

#endif // STELLUX_DRIVERS_USB_HID_HID_PARSER_H
