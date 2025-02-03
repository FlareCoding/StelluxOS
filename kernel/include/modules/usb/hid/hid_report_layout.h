#ifndef HID_REPORT_LAYOUT_H
#define HID_REPORT_LAYOUT_H
#include "hid_report_item.h"
#include <kstl/vector.h> 

namespace hid {
// Tracks global and local state while parsing the HID report descriptor.
struct parsing_context {
    uint16_t current_bit_offset = 0;  // Tracks the next available bit offset
    uint8_t report_size = 0;          // Number of bits per field
    uint8_t report_count = 0;         // Number of fields for this item
    uint16_t usage_page = 0;          // Current usage page
    kstl::vector<uint16_t> usages;    // List of usages collected from Local Items
    uint16_t usage_minimum;           // Usage minimum value for sequential values
    uint16_t usage_maximum;           // Usage maximum value for sequential values

    // Reset local-specific context after processing a Main Item
    inline void reset_local_context() {
        usages.clear();
        usage_minimum = 0;
        usage_maximum = 0;
    }
};

// Represents the layout of a single input/output field within the HID report.
struct field_info {
    uint16_t bit_offset;        // Starting bit offset within the report
    uint8_t bit_size;           // Size of the field in bits
    uint16_t usage_page;        // Usage page associated with the field
    uint16_t usage;             // Specific usage (e.g., generic input)
    bool is_array;              // Indicates if this field is an array (e.g., for keyboards)

    field_info(uint16_t offset, uint8_t size, uint16_t page, uint16_t usage_val, bool array = false)
        : bit_offset(offset), bit_size(size), usage_page(page), usage(usage_val), is_array(array) {}
};

// This class parses a HID report descriptor and stores a layout of input/output fields.
class hid_report_layout {
public:
    // Constructor that parses the report items
    hid_report_layout(const kstl::vector<hid_report_item>& items);

    // Accessors for querying the layout
    const field_info* find_field_by_usage(uint16_t usage_page, uint16_t usage) const;

    // Access the full list of parsed fields
    const kstl::vector<field_info>& get_fields() const { return fields; }

    // Retrieves how many bits are used for a usage page
    uint16_t get_total_bits_for_usage_page(uint16_t usage_page) const;

private:
    kstl::vector<field_info> fields;  // Vector storing information about parsed fields

    // Internal method to parse the given report items
    void _parse_items(const kstl::vector<hid_report_item>& items);

    // Helper to process a Main Item and store its layout
    void _process_main_item(const hid_report_item& main_item, parsing_context& context);
};

}  // namespace hid

#endif  // HID_REPORT_LAYOUT_H
