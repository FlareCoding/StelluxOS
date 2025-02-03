#ifndef HID_CONSTANTS_H
#define HID_CONSTANTS_H
#include <types.h>

namespace hid {
// -----------------------------------------------------------------------------
// HID Item Types and Tags
// -----------------------------------------------------------------------------
// Item types define the kind of information contained in a report descriptor item.
enum class item_type : uint8_t {
    main     = 0x0,  // Main item: input/output/feature items
    global   = 0x1,  // Global item: report size, usage page, etc.
    local    = 0x2,  // Local item: usage, usage minimum/maximum
    reserved = 0x3   // Reserved type
};

// Main item tags (used in Main item type)
enum class main_item_tag : uint8_t {
    input    = 0x8,  // Input item
    output   = 0x9,  // Output item
    feature  = 0xB,  // Feature item
    collection = 0xA,  // Start a collection (Application, Physical, etc.)
    end_collection = 0xC  // End a collection
};

// Global item tags (used in Global item type)
enum class global_item_tag : uint8_t {
    usage_page        = 0x0,  // Usage Page (what category of input)
    logical_minimum   = 0x1,  // Minimum logical value
    logical_maximum   = 0x2,  // Maximum logical value
    physical_minimum  = 0x3,  // Minimum physical value
    physical_maximum  = 0x4,  // Maximum physical value
    unit_exponent     = 0x5,  // Unit exponent
    unit              = 0x6,  // Unit system (SI units)
    report_size       = 0x7,  // Number of bits for each data field
    report_id         = 0x8,  // Report ID (used for multi-report devices)
    report_count      = 0x9,  // Number of fields in the report
    push              = 0xA,  // Push context onto stack
    pop               = 0xB   // Pop context from stack
};

// Local item tags (used in Local item type)
enum class local_item_tag : uint8_t {
    usage             = 0x0,  // Specific usage within a usage page (e.g., X-axis, button)
    usage_minimum     = 0x1,  // Minimum usage value
    usage_maximum     = 0x2,  // Maximum usage value
    designator_index  = 0x3,  // Designator index (optional physical index)
    designator_minimum = 0x4, // Minimum designator index
    designator_maximum = 0x5, // Maximum designator index
    string_index      = 0x7,  // String index (optional string descriptor)
    string_minimum    = 0x8,  // Minimum string index
    string_maximum    = 0x9,  // Maximum string index
    delimiter         = 0xA   // Used to delimit items in compound reports
};

// -----------------------------------------------------------------------------
// Common Usage Pages and Usages
// -----------------------------------------------------------------------------
// Common usage pages
enum class usage_page : uint16_t {
    generic_desktop = 0x1,  // Generic Desktop Controls (e.g., mouse, keyboard, joystick)
    simulation      = 0x2,  // Simulation Controls (e.g., flight controls)
    vr_controls     = 0x3,  // Virtual Reality Controls
    sport_controls  = 0x4,  // Sports Controls
    game_controls   = 0x5,  // Game Controls
    generic_device  = 0x6,  // Generic Device Controls
    keyboard        = 0x7,  // Keyboard/Keypad
    leds            = 0x8,  // LED indicators
    buttons         = 0x9,  // Button inputs
    ordinal         = 0xA,  // Ordinal (device index tracking)
    telephony       = 0xB,  // Telephony Devices
    consumer        = 0xC,  // Consumer Controls (e.g., multimedia keys)
    digitizer       = 0xD   // Digitizers (e.g., touch screens)
};

// Common usages within the Generic Desktop usage page
enum class generic_desktop_usage : uint8_t {
    pointer          = 0x01,  // Pointer (e.g., mouse pointer)
    mouse            = 0x02,  // Mouse
    joystick         = 0x04,  // Joystick
    gamepad          = 0x05,  // Gamepad
    keyboard         = 0x06,  // Keyboard
    keypad           = 0x07,  // Keypad
    multi_axis_controller = 0x08,  // Multi-axis controller
    x_axis           = 0x30,  // X-axis movement
    y_axis           = 0x31,  // Y-axis movement
    z_axis           = 0x32,  // Z-axis movement
    wheel            = 0x38,  // Scroll wheel
    hat_switch       = 0x39   // Hat switch (D-pad)
};

// -----------------------------------------------------------------------------
// HID Report Descriptor Utilities
// -----------------------------------------------------------------------------
inline const char* to_string(item_type type) {
    switch (type) {
        case item_type::main: return "Main";
        case item_type::global: return "Global";
        case item_type::local: return "Local";
        default: return "Reserved";
    }
}

inline const char* to_string(main_item_tag tag) {
    switch (tag) {
        case main_item_tag::input: return "Input";
        case main_item_tag::output: return "Output";
        case main_item_tag::feature: return "Feature";
        case main_item_tag::collection: return "Collection";
        case main_item_tag::end_collection: return "End Collection";
        default: return "Unknown";
    }
}

inline const char* to_string(global_item_tag tag) {
    switch (tag) {
        case global_item_tag::usage_page: return "Usage Page";
        case global_item_tag::logical_minimum: return "Logical Minimum";
        case global_item_tag::logical_maximum: return "Logical Maximum";
        case global_item_tag::physical_minimum: return "Physical Minimum";
        case global_item_tag::physical_maximum: return "Physical Maximum";
        case global_item_tag::unit_exponent: return "Unit Exponent";
        case global_item_tag::unit: return "Unit";
        case global_item_tag::report_size: return "Report Size";
        case global_item_tag::report_id: return "Report ID";
        case global_item_tag::report_count: return "Report Count";
        case global_item_tag::push: return "Push";
        case global_item_tag::pop: return "Pop";
        default: return "Unknown";
    }
}

inline const char* to_string(local_item_tag tag) {
    switch (tag) {
        case local_item_tag::usage: return "Usage";
        case local_item_tag::usage_minimum: return "Usage Minimum";
        case local_item_tag::usage_maximum: return "Usage Maximum";
        case local_item_tag::designator_index: return "Designator Index";
        case local_item_tag::designator_minimum: return "Designator Minimum";
        case local_item_tag::designator_maximum: return "Designator Maximum";
        case local_item_tag::string_index: return "String Index";
        case local_item_tag::string_minimum: return "String Minimum";
        case local_item_tag::string_maximum: return "String Maximum";
        case local_item_tag::delimiter: return "Delimiter";
        default: return "Unknown";
    }
}

}  // namespace hid

#endif  // HID_CONSTANTS_H