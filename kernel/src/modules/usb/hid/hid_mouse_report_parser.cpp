#include <modules/usb/hid/hid_mouse_report_parser.h>
#include <serial/serial.h>

// State variables to track parsing context
struct parser_state {
    uint8_t report_offset = 0;  // Tracks the current bit offset in the report
    uint16_t usage_page = 0;    // Tracks the current usage page
    uint8_t report_size = 0;    // Size of the current item in bits
    uint8_t report_count = 0;   // Number of items for the current input
    kstl::vector<uint32_t> local_usages;  // Tracks collected local usages for current input
};

// Helper function to print information about an item
static void print_item_info(const hid_report_item& item, const parser_state& state, const char* description) {
    serial::printf("Item: Type=%d, Tag=%d, Data=0x%x | %s\n", item.type, item.tag, item.data, description);
    serial::printf("  Current Offset: %d bits, Report Size: %d bits, Report Count: %d\n", 
                   state.report_offset, state.report_size, state.report_count);
}

// Main function to parse the mouse layout
bool parse_mouse_report_layout(const kstl::vector<hid_report_item>& items,
                               hid_mouse_report_layout& layout) {

    __unused layout;

    parser_state state;  // Initialize parsing state

    for (const auto& item : items) {
        switch (item.type) {
            case hid_item_type_global:
                switch (item.tag) {
                    case 0x0:  // Usage Page
                        state.usage_page = static_cast<uint16_t>(item.data);
                        print_item_info(item, state, "Global: Usage Page");
                        break;
                    case 0x7:  // Report Size
                        state.report_size = static_cast<uint8_t>(item.data);
                        print_item_info(item, state, "Global: Report Size");
                        break;
                    case 0x9:  // Report Count
                        state.report_count = static_cast<uint8_t>(item.data);
                        print_item_info(item, state, "Global: Report Count");
                        break;
                    case 0x1:  // Logical Minimum
                        print_item_info(item, state, "Global: Logical Minimum");
                        break;
                    case 0x2:  // Logical Maximum
                        print_item_info(item, state, "Global: Logical Maximum");
                        break;
                }
                break;

            case hid_item_type_local:
                if (item.tag == 0x0) {  // Usage
                    state.local_usages.push_back(item.data);
                    print_item_info(item, state, "Local: Usage");
                    if (state.usage_page == 0x1) {
                        if (item.data == 0x30) {
                            serial::printf("  -> Detected X-axis\n");
                        } else if (item.data == 0x31) {
                            serial::printf("  -> Detected Y-axis\n");
                        } else if (item.data == 0x38) {
                            serial::printf("  -> Detected Wheel\n");
                        }
                    } else if (state.usage_page == 0x9) {
                        serial::printf("  -> Detected Button-related Usage\n");
                    }
                }
                break;

            case hid_item_type_main:
                if (item.tag == 0x8) {  // Input item
                    uint8_t field_size = ((state.report_count * state.report_size) + 7) / 8;  // Field size in bytes
                    print_item_info(item, state, "Main: Input");

                    serial::printf("  Local Usages for this input: ");
                    for (auto usage : state.local_usages) {
                        serial::printf("0x%x ", usage);
                    }
                    serial::printf("\n");

                    if (state.usage_page == 0x9) {  // Buttons
                        serial::printf("  -> Button input field at offset %d bits, size %d bits\n", 
                                       state.report_offset, field_size * 8);
                    } else if (state.usage_page == 0x1) {  // Movement axes
                        serial::printf("  -> Movement data (X/Y/Wheel) at offset %d bits, size %d bits\n", 
                                       state.report_offset, field_size * 8);
                    }
                    state.report_offset += field_size * 8;  // Move the offset forward
                    state.local_usages.clear();  // Clear local usages for the next input
                }
                break;
        }
    }

    serial::printf("\nFinal Report Layout Summary:\n");
    serial::printf("  Total report size: %d bits\n", state.report_offset);

    return false;
}

