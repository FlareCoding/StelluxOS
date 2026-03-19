#include "drivers/usb/hid/hid_keyboard_handler.h"
#include "drivers/usb/hid/hid_constants.h"
#include "common/logging.h"
#include "common/string.h"

namespace usb::hid {

// USB HID scancode to key name (US layout)
static const char* scancode_to_name_lookup(uint8_t sc) {
    switch (sc) {
    case 0x04: return "A";     case 0x05: return "B";
    case 0x06: return "C";     case 0x07: return "D";
    case 0x08: return "E";     case 0x09: return "F";
    case 0x0A: return "G";     case 0x0B: return "H";
    case 0x0C: return "I";     case 0x0D: return "J";
    case 0x0E: return "K";     case 0x0F: return "L";
    case 0x10: return "M";     case 0x11: return "N";
    case 0x12: return "O";     case 0x13: return "P";
    case 0x14: return "Q";     case 0x15: return "R";
    case 0x16: return "S";     case 0x17: return "T";
    case 0x18: return "U";     case 0x19: return "V";
    case 0x1A: return "W";     case 0x1B: return "X";
    case 0x1C: return "Y";     case 0x1D: return "Z";
    case 0x1E: return "1";     case 0x1F: return "2";
    case 0x20: return "3";     case 0x21: return "4";
    case 0x22: return "5";     case 0x23: return "6";
    case 0x24: return "7";     case 0x25: return "8";
    case 0x26: return "9";     case 0x27: return "0";
    case 0x28: return "ENTER";     case 0x29: return "ESC";
    case 0x2A: return "BACKSPACE"; case 0x2B: return "TAB";
    case 0x2C: return "SPACE";     case 0x2D: return "MINUS";
    case 0x2E: return "EQUAL";     case 0x2F: return "LBRACKET";
    case 0x30: return "RBRACKET";  case 0x31: return "BACKSLASH";
    case 0x33: return "SEMICOLON"; case 0x34: return "APOSTROPHE";
    case 0x35: return "GRAVE";     case 0x36: return "COMMA";
    case 0x37: return "DOT";       case 0x38: return "SLASH";
    case 0x39: return "CAPSLOCK";
    case 0x3A: return "F1";  case 0x3B: return "F2";
    case 0x3C: return "F3";  case 0x3D: return "F4";
    case 0x3E: return "F5";  case 0x3F: return "F6";
    case 0x40: return "F7";  case 0x41: return "F8";
    case 0x42: return "F9";  case 0x43: return "F10";
    case 0x44: return "F11"; case 0x45: return "F12";
    case 0x49: return "INSERT";  case 0x4A: return "HOME";
    case 0x4B: return "PGUP";   case 0x4C: return "DELETE";
    case 0x4D: return "END";    case 0x4E: return "PGDN";
    case 0x4F: return "RIGHT";  case 0x50: return "LEFT";
    case 0x51: return "DOWN";   case 0x52: return "UP";
    default: return nullptr;
    }
}

static const char* modifier_names[8] = {
    "LCTRL", "LSHIFT", "LALT", "LGUI",
    "RCTRL", "RSHIFT", "RALT", "RGUI",
};

static const field_info* find_field(const report_layout& layout,
                                    uint16_t usage_pg, uint16_t usage_val) {
    for (uint16_t i = 0; i < layout.num_fields; i++) {
        if (layout.fields[i].usage_page == usage_pg &&
            layout.fields[i].usage == usage_val) {
            return &layout.fields[i];
        }
    }
    return nullptr;
}

void hid_keyboard_handler::init(const report_layout& layout) {
    auto kb = static_cast<uint16_t>(usage_page::keyboard);

    // Modifiers are typically the first field on the keyboard usage page
    // (Usage Min 0xE0 to Usage Max 0xE7 = 8 modifier bits)
    auto* mod_field = find_field(layout, kb, 0xE0);
    if (mod_field) {
        m_layout.modifier_offset = mod_field->bit_offset;
        // Count consecutive keyboard-page fields starting at 0xE0
        uint16_t mod_bits = 0;
        for (uint16_t i = 0; i < layout.num_fields; i++) {
            if (layout.fields[i].usage_page == kb &&
                layout.fields[i].usage >= 0xE0 && layout.fields[i].usage <= 0xE7) {
                mod_bits += layout.fields[i].bit_size;
            }
        }
        m_layout.modifier_size = mod_bits;
    }

    // Keycodes: find the first non-modifier keyboard field (usage 0x00-0xDF range)
    // These are typically 6 slots of 8 bits each (boot keyboard layout)
    for (uint16_t i = 0; i < layout.num_fields; i++) {
        auto& f = layout.fields[i];
        if (f.usage_page == kb && f.usage < 0xE0) {
            m_layout.keycode_offset = f.bit_offset;
            m_layout.keycode_size = f.bit_size;
            // Count how many consecutive keycode slots exist
            uint8_t count = 0;
            for (uint16_t j = i; j < layout.num_fields; j++) {
                if (layout.fields[j].usage_page == kb &&
                    layout.fields[j].usage < 0xE0 &&
                    layout.fields[j].bit_size == f.bit_size) {
                    count++;
                } else {
                    break;
                }
            }
            m_layout.keycode_count = count > 6 ? 6 : count;
            break;
        }
    }

    log::info("hid-kbd: modifiers=%u bits @ %u, keycodes=%u slots of %u bits @ %u",
              m_layout.modifier_size, m_layout.modifier_offset,
              m_layout.keycode_count, m_layout.keycode_size, m_layout.keycode_offset);
}

void hid_keyboard_handler::on_report(const uint8_t* data, uint32_t) {
    // Read modifier byte
    uint8_t modifiers = 0;
    if (m_layout.modifier_size > 0) {
        uint32_t byte_off = m_layout.modifier_offset / 8;
        modifiers = data[byte_off];
    }

    // Detect modifier changes
    uint8_t mod_changed = modifiers ^ m_prev_modifiers;
    if (mod_changed) {
        for (int b = 0; b < 8; b++) {
            if (mod_changed & (1u << b)) {
                bool pressed = (modifiers & (1u << b)) != 0;
                log::info("hid-kbd: %s %s", modifier_names[b], pressed ? "pressed" : "released");
            }
        }
        m_prev_modifiers = modifiers;
    }

    // Read keycodes (each slot is keycode_size bits, typically 8)
    uint8_t keycodes[6] = {};
    for (uint8_t i = 0; i < m_layout.keycode_count; i++) {
        uint32_t bit_off = m_layout.keycode_offset + i * m_layout.keycode_size;
        uint32_t byte_off = bit_off / 8;
        keycodes[i] = data[byte_off];
    }

    // Detect newly pressed keys (in current but not in previous)
    for (uint8_t i = 0; i < m_layout.keycode_count; i++) {
        if (keycodes[i] == 0) {
            continue;
        }
        bool was_pressed = false;
        for (uint8_t j = 0; j < 6; j++) {
            if (m_prev_keycodes[j] == keycodes[i]) {
                was_pressed = true;
                break;
            }
        }
        if (!was_pressed) {
            const char* name = scancode_to_name(keycodes[i]);
            log::info("hid-kbd: key pressed: %s (0x%02x)", name ? name : "?", keycodes[i]);
        }
    }

    // Detect released keys (in previous but not in current)
    for (uint8_t i = 0; i < 6; i++) {
        if (m_prev_keycodes[i] == 0) {
            continue;
        }
        bool still_pressed = false;
        for (uint8_t j = 0; j < m_layout.keycode_count; j++) {
            if (keycodes[j] == m_prev_keycodes[i]) {
                still_pressed = true;
                break;
            }
        }
        if (!still_pressed) {
            const char* name = scancode_to_name(m_prev_keycodes[i]);
            log::info("hid-kbd: key released: %s (0x%02x)", name ? name : "?", m_prev_keycodes[i]);
        }
    }

    string::memcpy(m_prev_keycodes, keycodes, 6);
}

const char* hid_keyboard_handler::scancode_to_name(uint8_t scancode) {
    return scancode_to_name_lookup(scancode);
}

} // namespace usb::hid
