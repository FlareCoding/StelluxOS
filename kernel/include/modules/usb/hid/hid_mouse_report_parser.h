#ifndef MOUSE_REPORT_PARSER_H
#define MOUSE_REPORT_PARSER_H
#include "hid_report_parser.h"

// Structure that defines the layout of a mouse report.
struct hid_mouse_report_layout {
    uint8_t buttons_offset;  // Offset for the button field
    uint8_t buttons_size;    // Size for the button field
    uint8_t dx_offset;       // Offset for the X-axis field
    uint8_t dx_size;         // Size for the X-axis field
    uint8_t dy_offset;       // Offset for the Y-axis field
    uint8_t dy_size;         // Size for the Y-axis field
};

// Parse a vector of universal HID items into a mouse report layout.
// Returns true if the layout was successfully extracted.
bool parse_mouse_report_layout(const kstl::vector<hid_report_item>& items,
                               hid_mouse_report_layout& layout);

#endif // MOUSE_REPORT_PARSER_H
