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

enum input_field_flags : uint16_t {
    input_field_constant = 1u << 0,
    input_field_variable = 1u << 1,
    input_field_relative = 1u << 2,
};

// A parsed field within one input report body. bit_offset is relative to the
// start of the report body, not including any leading Report ID byte on wire.
struct field_info {
    uint32_t bit_offset = 0;
    uint16_t bit_size = 0;
    uint8_t  report_id = 0;
    uint16_t usage_page = 0;
    uint16_t usage = 0;
    uint16_t input_flags = 0;
    int32_t  logical_minimum = 0;
    int32_t  logical_maximum = 0;

    inline bool is_constant() const {
        return (input_flags & input_field_constant) != 0;
    }

    inline bool is_variable() const {
        return (input_flags & input_field_variable) != 0;
    }

    inline bool is_relative() const {
        return (input_flags & input_field_relative) != 0;
    }
};

struct input_report_info {
    uint8_t  report_id = 0;
    uint32_t byte_length = 0; // Report body length, excluding any Report ID byte.
    uint16_t field_begin = 0;
    uint16_t field_count = 0;
};

// Parsed input-report layout from a HID report descriptor.
// Owns heap-allocated arrays; call destroy() when done.
struct report_layout {
    field_info*        fields = nullptr;
    uint16_t           num_fields = 0;
    input_report_info* input_reports = nullptr;
    uint16_t           num_input_reports = 0;
    bool               uses_report_ids = false;
    uint32_t           max_input_report_bytes = 0;

    void destroy();
};

const input_report_info* find_input_report(const report_layout& layout, uint8_t report_id);

inline const field_info* report_fields(const report_layout& layout,
                                       const input_report_info& report) {
    if (!layout.fields || report.field_begin >= layout.num_fields) {
        return nullptr;
    }
    return layout.fields + report.field_begin;
}

// Parse a raw HID report descriptor into a report_layout.
// The layout's fields array is heap-allocated; caller must call layout.destroy().
// Returns 0 on success, negative on failure.
int32_t parse_report_descriptor(const uint8_t* descriptor, size_t length,
                                report_layout& out);

} // namespace usb::hid

#endif // STELLUX_DRIVERS_USB_HID_HID_PARSER_H
