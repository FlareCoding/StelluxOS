#ifndef HID_REPORT_PARSER_H
#define HID_REPORT_PARSER_H
#include "hid_report_layout.h"

namespace hid {
// Parses a raw HID report descriptor into individual items for further processing.
class hid_report_parser {
public:
    // Parses the given report descriptor buffer and fills the items vector.
    // Returns true if the parsing is successful; false otherwise.
    static bool parse_descriptor(const uint8_t* report, size_t length, kstl::vector<hid_report_item>& items);
};
} // namespace hid

#endif // HID_REPORT_PARSER_H
