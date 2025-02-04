#include <drivers/usb/hid/hid_report_parser.h>
#include <serial/serial.h>

namespace hid {
bool hid_report_parser::parse_descriptor(const uint8_t* report, size_t length, kstl::vector<hid_report_item>& items) {
    size_t index = 0; // Tracks the current byte in the report descriptor

    while (index < length) {
        if (index >= length) {
            serial::printf("[HID Parser Error] Out of bounds access at index %zu\n", index);
            return false; // Prevent buffer overrun
        }

        // Read the first byte to extract type, tag, and size
        uint8_t prefix = report[index++];
        uint8_t size = prefix & 0x03; // Size is in the last 2 bits (00 = 0 bytes, 01 = 1 byte, etc.)
        uint8_t type = (prefix >> 2) & 0x03; // Bits [3:2] represent the type
        uint8_t tag = (prefix >> 4) & 0x0F; // Bits [7:4] represent the tag

        // Handle the special case: size == 3 means the item data size is 4 bytes
        if (size == 3) size = 4;

        // Read the next 'size' bytes as the data payload
        uint32_t data = 0;
        for (uint8_t i = 0; i < size; ++i) {
            if (index < length) {
                data |= (report[index++] << (8 * i)); // Accumulate bytes into data (little-endian)
            } else {
                serial::printf("[HID Parser Error] Unexpected end of report descriptor at index %zu\n", index);
                return false;
            }
        }

        // Determine the item type and create a corresponding hid_report_item
        hid_report_item item(static_cast<item_type>(type), tag, size, data);
        items.push_back(item);
    }

    return true;  // Successfully parsed all items
}
}  // namespace hid

