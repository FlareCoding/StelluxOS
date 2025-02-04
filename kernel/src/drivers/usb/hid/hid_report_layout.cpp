#include <drivers/usb/hid/hid_report_layout.h>
#include <serial/serial.h>

namespace hid {
hid_report_layout::hid_report_layout(const kstl::vector<hid_report_item>& items) {
    _parse_items(items);
}

const field_info* hid_report_layout::find_field_by_usage(uint16_t usage_page, uint16_t usage) const {
    for (const auto& field : fields) {
        if (field.usage_page == usage_page && field.usage == usage) {
            return &field;
        }
    }
    return nullptr;  // Field not found
}

uint16_t hid_report_layout::get_total_bits_for_usage_page(uint16_t usage_page) const {
    uint16_t total_bits = 0;

    for (const auto& field : fields) {
        if (field.usage_page == usage_page) {
            total_bits += field.bit_size;
        }
    }

    return total_bits;
}

void hid_report_layout::_parse_items(const kstl::vector<hid_report_item>& items) {
    parsing_context context;

    for (const auto& item : items) {
        switch (item.type) {
            case item_type::global:
                switch (static_cast<global_item_tag>(item.tag)) {
                    case global_item_tag::usage_page:
                        context.usage_page = static_cast<uint16_t>(item.data);
                        break;
                    case global_item_tag::report_size:
                        context.report_size = static_cast<uint8_t>(item.data);
                        break;
                    case global_item_tag::report_count:
                        context.report_count = static_cast<uint8_t>(item.data);
                        break;
                    default:
                        break;
                }
                break;

            case item_type::local:
                if (static_cast<local_item_tag>(item.tag) == local_item_tag::usage) {
                    context.usages.push_back(static_cast<uint16_t>(item.data));
                } else if (static_cast<local_item_tag>(item.tag) == local_item_tag::usage_minimum) {
                    context.usage_minimum = static_cast<uint16_t>(item.data);
                } else if (static_cast<local_item_tag>(item.tag) == local_item_tag::usage_maximum) {
                    context.usage_maximum = static_cast<uint16_t>(item.data);

                    // Clear previous usages before expanding the range
                    context.usages.clear();

                    // Expand the range [usage_minimum, usage_maximum] into individual usages
                    for (uint16_t usage = context.usage_minimum; usage <= context.usage_maximum; ++usage) {
                        context.usages.push_back(usage);
                    }
                }
                break;

            case item_type::main:
                if (static_cast<main_item_tag>(item.tag) == main_item_tag::input) {
                    _process_main_item(item, context);
                    context.reset_local_context();
                }
                break;

            default:
                break;
        }
    }
}

void hid_report_layout::_process_main_item(const hid_report_item& main_item, parsing_context& context) {
    bool is_constant = main_item.data & 0x01;
    if (is_constant) {
        context.current_bit_offset += context.report_size * context.report_count;
        return;
    }

    uint16_t field_size_bits = context.report_size;
    uint16_t current_offset = context.current_bit_offset;

    for (uint8_t i = 0; i < context.report_count; ++i) {
        uint16_t usage = (i < context.usages.size()) ? context.usages[i] : context.usages.back();  // Handle fewer usages

#if 0
        serial::printf("[HID Layout] Assigning field: Offset=%d, Size=%d bits, Usage Page=0x%x, Usage=0x%x\n",
                       current_offset, field_size_bits, context.usage_page, usage);
#endif

        fields.push_back(field_info(current_offset, field_size_bits, context.usage_page, usage));

        current_offset += field_size_bits;
    }

    context.current_bit_offset = current_offset;
}
} // namespace hid
