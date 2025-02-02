#include <modules/usb/hid/hid_report_parser.h>
#include <serial/serial.h>

const char* get_item_type_str(uint8_t type) {
    switch (type) {
        case hid_item_type_main:     return "Main";
        case hid_item_type_global:   return "Global";
        case hid_item_type_local:    return "Local";
        case hid_item_type_reserved: return "Reserved";
        default: return "Unknown";
    }
}

const char* get_main_item_tag_str(uint8_t tag) {
    switch (tag) {
        case 8:  return "Input";
        case 9:  return "Output";
        case 10: return "Collection";
        case 11: return "Feature";
        case 12: return "End Collection";
        default: return "Unknown Main Tag";
    }
}

const char* get_global_item_tag_str(uint8_t tag) {
    switch (tag) {
        case 0:  return "Usage Page";
        case 1:  return "Logical Minimum";
        case 2:  return "Logical Maximum";
        case 3:  return "Physical Minimum";
        case 4:  return "Physical Maximum";
        case 5:  return "Unit Exponent";
        case 6:  return "Unit";
        case 7:  return "Report Size";
        case 8:  return "Report ID";
        case 9:  return "Report Count";
        case 10: return "Push";
        case 11: return "Pop";
        default: return "Unknown Global Tag";
    }
}

const char* get_local_item_tag_str(uint8_t tag) {
    switch (tag) {
        case 0: return "Usage";
        case 1: return "Usage Minimum";
        case 2: return "Usage Maximum";
        default: return "Unknown Local Tag";
    }
}

bool hid_report_parser::parse_descriptor(const uint8_t* report, size_t length,
                                         kstl::vector<hid_report_item>& items) {
    size_t offset = 0;
    while (offset < length) {
        uint8_t prefix = report[offset++];

        // Check for long item indicator (0xFE)
        if (prefix == 0xFE) {
            if (offset >= length) {
                serial::printf("Error: Incomplete long item header\n");
                return false;
            }
            uint8_t long_item_length = report[offset++];

            uint8_t long_item_tag = report[offset++];
            __unused long_item_tag;

            offset += long_item_length;
            continue;
        }

        // For a short item, the lower 2 bits encode the data size.
        uint8_t size_code = prefix & 0x03;
        uint8_t data_size = (size_code == 3) ? 4 : size_code;

        // Bits 2-3 represent the type; bits 4-7 represent the tag.
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = prefix >> 4;

        if (offset + data_size > length) {
            serial::printf("Error: Not enough data for item (offset %zu, data_size %u, total length %zu)\n",
                   offset, data_size, length);
            return false;
        }

        uint32_t data = 0;
        if (data_size > 0) {
            memcpy(&data, report + offset, data_size);
            offset += data_size;
        }

        hid_report_item item;
        item.type = type;
        item.tag  = tag;
        item.size = data_size;
        item.data = data;
        items.push_back(item);
    }

#if 1
    // Print parsed items with collection indentation.
    serial::printf("Parsed HID Report Descriptor Items:\n");
    int indent = 0;
    for (size_t i = 0; i < items.size(); i++) {
        const hid_report_item &item = items[i];
        if (item.type == hid_item_type_main && item.tag == 10) { // Collection
            for (int j = 0; j < indent; j++) serial::printf("  ");
            serial::printf("Collection: Tag=%s, Data=0x%x\n", get_main_item_tag_str(item.tag), item.data);
            indent++;
        } else if (item.type == hid_item_type_main && item.tag == 12) { // End Collection
            indent = (indent > 0) ? indent - 1 : 0;
            for (int j = 0; j < indent; j++) serial::printf("  ");
            serial::printf("End Collection: Tag=%s\n", get_main_item_tag_str(item.tag));
        } else {
            for (int j = 0; j < indent; j++) serial::printf("  ");
            const char* type_str = get_item_type_str(item.type);
            const char* tag_str = "Unknown";
            if (item.type == hid_item_type_main)
                tag_str = get_main_item_tag_str(item.tag);
            else if (item.type == hid_item_type_global)
                tag_str = get_global_item_tag_str(item.tag);
            else if (item.type == hid_item_type_local)
                tag_str = get_local_item_tag_str(item.tag);
            serial::printf("%s Item: Tag=%s (0x%x), Size=%u, Data=0x%x\n",
                   type_str, tag_str, item.tag, item.size, item.data);
        }
    }
#endif

    return true;
}
