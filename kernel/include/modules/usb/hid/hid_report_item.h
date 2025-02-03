#ifndef HID_REPORT_ITEM_H
#define HID_REPORT_ITEM_H
#include "hid_constants.h"

namespace hid {
// Represents an individual item within a HID report descriptor.
class hid_report_item {
public:
    item_type type;     // Type of the item (main, global, or local)
    uint8_t tag;        // Specific tag within the item type (e.g., Usage, Report Size)
    uint8_t size;       // Size of the item's data (in bytes)
    uint32_t data;      // Data payload (stored in 32 bits for convenience)

    // Constructor to initialize the report item
    hid_report_item(item_type item_type, uint8_t item_tag, uint8_t item_size, uint32_t item_data)
        : type(item_type), tag(item_tag), size(item_size), data(item_data) {}


    const char* get_tag_str() const;

    // Utility function to print the item for debugging purposes
    void print() const;
};
}  // namespace hid

#endif  // HID_REPORT_ITEM_H
