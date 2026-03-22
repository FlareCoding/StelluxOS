#include "drivers/usb/hid/hid_keyboard_handler.h"
#include "drivers/usb/hid/hid_constants.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"

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

bool hid_keyboard_handler::is_modifier_usage(uint16_t usage) {
    return usage >= 0xE0u && usage <= 0xE7u;
}

bool hid_keyboard_handler::is_reserved_array_usage(uint16_t usage) {
    return usage >= 0x01u && usage <= 0x03u;
}

hid_keyboard_handler::~hid_keyboard_handler() {
    reset_state();
}

void hid_keyboard_handler::reset_state() {
    if (m_key_fields) {
        heap::ufree(m_key_fields);
        m_key_fields = nullptr;
    }
    if (m_prev_keycodes) {
        heap::ufree(m_prev_keycodes);
        m_prev_keycodes = nullptr;
    }
    if (m_curr_keycodes) {
        heap::ufree(m_curr_keycodes);
        m_curr_keycodes = nullptr;
    }

    for (uint8_t i = 0; i < 8; i++) {
        m_modifier_fields[i] = nullptr;
    }

    m_key_field_count = 0;
    m_prev_modifiers = 0;
    m_report_id = 0;
    m_ready = false;
}

bool hid_keyboard_handler::contains_keycode(const uint16_t* keycodes,
                                            uint16_t count, uint16_t keycode) {
    if (!keycodes || keycode == 0) {
        return false;
    }

    for (uint16_t i = 0; i < count; i++) {
        if (keycodes[i] == keycode) {
            return true;
        }
    }
    return false;
}

int32_t hid_keyboard_handler::init(const report_layout& layout,
                                   const input_report_info& report) {
    reset_state();

    auto kb = static_cast<uint16_t>(usage_page::keyboard);
    const field_info* fields = report_fields(layout, report);
    if (!fields || report.field_count == 0) {
        return -1;
    }

    uint16_t key_field_count = 0;
    for (uint16_t i = 0; i < report.field_count; i++) {
        const auto& field = fields[i];
        if (field.usage_page != kb || field.is_constant()) {
            continue;
        }

        if (field.is_variable() && is_modifier_usage(field.usage)) {
            if (field.usage >= 0xE0 && field.usage <= 0xE7) {
                m_modifier_fields[field.usage - 0xE0] = &field;
            }
            continue;
        }

        key_field_count++;
    }

    if (key_field_count == 0) {
        reset_state();
        return -1;
    }

    m_key_fields = static_cast<const field_info**>(
        heap::ualloc(key_field_count * sizeof(field_info*)));
    m_prev_keycodes = static_cast<uint16_t*>(
        heap::ualloc(key_field_count * sizeof(uint16_t)));
    m_curr_keycodes = static_cast<uint16_t*>(
        heap::ualloc(key_field_count * sizeof(uint16_t)));
    if (!m_key_fields || !m_prev_keycodes || !m_curr_keycodes) {
        reset_state();
        return -1;
    }

    uint16_t slot = 0;
    for (uint16_t i = 0; i < report.field_count; i++) {
        const auto& field = fields[i];
        if (field.usage_page != kb || field.is_constant()) {
            continue;
        }
        if (field.is_variable() && is_modifier_usage(field.usage)) {
            continue;
        }
        if (slot < key_field_count) {
            m_key_fields[slot++] = &field;
        }
    }

    string::memset(m_prev_keycodes, 0, key_field_count * sizeof(uint16_t));
    string::memset(m_curr_keycodes, 0, key_field_count * sizeof(uint16_t));

    m_key_field_count = key_field_count;
    m_report_id = report.report_id;
    m_ready = true;

    uint8_t modifier_count = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (m_modifier_fields[i]) {
            modifier_count++;
        }
    }

    log::info("hid-kbd: report=%u modifiers=%u key-fields=%u",
              m_report_id, modifier_count, m_key_field_count);
    return 0;
}

void hid_keyboard_handler::on_report(const uint8_t* data, uint32_t length) {
    if (!m_ready) {
        return;
    }

    uint8_t modifiers = 0;
    for (uint8_t i = 0; i < 8; i++) {
        const auto* field = m_modifier_fields[i];
        if (!field) {
            continue;
        }

        if (read_field_unsigned(data, length, field->bit_offset, field->bit_size) != 0) {
            modifiers |= static_cast<uint8_t>(1u << i);
        }
    }

    uint8_t mod_changed = modifiers ^ m_prev_modifiers;
    if (mod_changed) {
        for (int b = 0; b < 8; b++) {
            if ((mod_changed & (1u << b)) != 0) {
                bool pressed = (modifiers & (1u << b)) != 0;
                log::info("hid-kbd: %s %s", modifier_names[b], pressed ? "pressed" : "released");
            }
        }
        m_prev_modifiers = modifiers;
    }

    for (uint16_t i = 0; i < m_key_field_count; i++) {
        const auto* field = m_key_fields[i];
        uint32_t raw = read_field_unsigned(data, length, field->bit_offset, field->bit_size);
        uint16_t keycode = 0;

        if (field->is_variable()) {
            keycode = raw != 0 ? field->usage : 0;
        } else if (raw <= 0xFFFFu) {
            keycode = static_cast<uint16_t>(raw);
            if (is_reserved_array_usage(keycode)) {
                keycode = 0;
            }
        }

        m_curr_keycodes[i] = keycode;
    }

    for (uint16_t i = 0; i < m_key_field_count; i++) {
        uint16_t keycode = m_curr_keycodes[i];
        if (keycode == 0 || contains_keycode(m_curr_keycodes, i, keycode)) {
            continue;
        }
        if (!contains_keycode(m_prev_keycodes, m_key_field_count, keycode)) {
            const char* name = scancode_to_name(keycode);
            log::info("hid-kbd: key pressed: %s (0x%04x)", name ? name : "?",
                      static_cast<uint32_t>(keycode));
        }
    }

    for (uint16_t i = 0; i < m_key_field_count; i++) {
        uint16_t keycode = m_prev_keycodes[i];
        if (keycode == 0 || contains_keycode(m_prev_keycodes, i, keycode)) {
            continue;
        }
        if (!contains_keycode(m_curr_keycodes, m_key_field_count, keycode)) {
            const char* name = scancode_to_name(keycode);
            log::info("hid-kbd: key released: %s (0x%04x)", name ? name : "?",
                      static_cast<uint32_t>(keycode));
        }
    }

    string::memcpy(m_prev_keycodes, m_curr_keycodes,
                   m_key_field_count * sizeof(uint16_t));
}

const char* hid_keyboard_handler::scancode_to_name(uint16_t scancode) {
    if (scancode > 0xFFu) {
        return nullptr;
    }
    return scancode_to_name_lookup(static_cast<uint8_t>(scancode));
}

} // namespace usb::hid
