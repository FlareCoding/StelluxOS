#include <drivers/usb/hid/hid_report_item.h>
#include <serial/serial.h>

namespace hid {
const char* hid_report_item::get_tag_str() const {
    switch (type) {
        case item_type::main:
            return to_string(static_cast<main_item_tag>(tag));
        case item_type::global:
            return to_string(static_cast<global_item_tag>(tag));
        case item_type::local:
            return to_string(static_cast<local_item_tag>(tag));
        default:
            return "Unknown Tag";
    }
}

void hid_report_item::print() const {
    serial::printf("[HID Report Item] Type=%s, Tag=%s, Size=%d bytes, Data=0x%X\n", 
                    to_string(type), get_tag_str(), size, data);
}
} // namespace hid
