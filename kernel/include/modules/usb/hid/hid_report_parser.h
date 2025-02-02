#ifndef HID_REPORT_PARSER_H
#define HID_REPORT_PARSER_H
#include <kstl/vector.h>

// -----------------------------------------------------------------------------
// Universal HID Report Parsing Types and Helper Functions
// -----------------------------------------------------------------------------
enum hid_item_type {
    hid_item_type_main     = 0,
    hid_item_type_global   = 1,
    hid_item_type_local    = 2,
    hid_item_type_reserved = 3
};

struct hid_report_item {
    uint8_t type;    // Main, Global, Local, etc.
    uint8_t tag;     // Specific tag (e.g., Input, Output, Collection, etc.)
    uint8_t size;    // Number of bytes in the data payload
    uint32_t data;   // Data payload (stored in 32 bits for convenience)
};

// Helper functions to convert type/tag to strings
const char* get_item_type_str(uint8_t type);
const char* get_main_item_tag_str(uint8_t tag);
const char* get_global_item_tag_str(uint8_t tag);
const char* get_local_item_tag_str(uint8_t tag);

// -----------------------------------------------------------------------------
// Universal HID Report Parser Class
// -----------------------------------------------------------------------------
class hid_report_parser {
public:
    // Parse a raw HID report descriptor into a vector of hid_report_item.
    // Returns true on success.
    static bool parse_descriptor(const uint8_t* report, size_t length,
                                 kstl::vector<hid_report_item>& items);
};

#endif // HID_REPORT_PARSER_H
