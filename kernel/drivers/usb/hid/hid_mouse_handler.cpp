#include "drivers/usb/hid/hid_mouse_handler.h"
#include "drivers/usb/hid/hid_constants.h"
#include "common/logging.h"
#include "common/string.h"

namespace usb::hid {

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

static uint16_t total_bits_for_page(const report_layout& layout, uint16_t usage_pg) {
    uint16_t total = 0;
    for (uint16_t i = 0; i < layout.num_fields; i++) {
        if (layout.fields[i].usage_page == usage_pg) {
            total += layout.fields[i].bit_size;
        }
    }
    return total;
}

void hid_mouse_handler::init(const report_layout& layout) {
    auto gd = static_cast<uint16_t>(usage_page::generic_desktop);
    auto btn = static_cast<uint16_t>(usage_page::buttons);

    // Buttons: find the first button field and use total bits for the page
    auto* btn_field = find_field(layout, btn, 1);
    if (btn_field) {
        m_layout.buttons_offset = btn_field->bit_offset;
        m_layout.buttons_size = total_bits_for_page(layout, btn);
    }

    // X axis
    auto* x_field = find_field(layout, gd, static_cast<uint16_t>(generic_desktop_usage::x_axis));
    if (x_field) {
        m_layout.x_offset = x_field->bit_offset;
        m_layout.x_size = x_field->bit_size;
    }

    // Y axis
    auto* y_field = find_field(layout, gd, static_cast<uint16_t>(generic_desktop_usage::y_axis));
    if (y_field) {
        m_layout.y_offset = y_field->bit_offset;
        m_layout.y_size = y_field->bit_size;
    }

    // Scroll wheel
    auto* wheel_field = find_field(layout, gd, static_cast<uint16_t>(generic_desktop_usage::wheel));
    if (wheel_field) {
        m_layout.wheel_offset = wheel_field->bit_offset;
        m_layout.wheel_size = wheel_field->bit_size;
    }

    log::info("hid-mouse: buttons=%u bits @ %u, x=%u bits @ %u, y=%u bits @ %u, wheel=%u bits @ %u",
              m_layout.buttons_size, m_layout.buttons_offset,
              m_layout.x_size, m_layout.x_offset,
              m_layout.y_size, m_layout.y_offset,
              m_layout.wheel_size, m_layout.wheel_offset);
}

void hid_mouse_handler::on_report(const uint8_t* data, uint32_t) {
    int32_t dx = 0, dy = 0, scroll = 0;
    uint32_t buttons = 0;

    if (m_layout.x_size > 0) {
        dx = read_signed_field(data, m_layout.x_offset, m_layout.x_size);
    }
    if (m_layout.y_size > 0) {
        dy = read_signed_field(data, m_layout.y_offset, m_layout.y_size);
    }
    if (m_layout.buttons_size > 0) {
        buttons = read_unsigned_field(data, m_layout.buttons_offset, m_layout.buttons_size);
    }
    if (m_layout.wheel_size > 0) {
        scroll = read_signed_field(data, m_layout.wheel_offset, m_layout.wheel_size);
    }

    if (dx != 0 || dy != 0) {
        log::debug("hid-mouse: dx=%d dy=%d", dx, dy);
    }

    uint32_t changed = buttons ^ m_prev_buttons;
    if (changed != 0) {
        for (int b = 0; b < 8; b++) {
            if (changed & (1u << b)) {
                bool pressed = (buttons & (1u << b)) != 0;
                log::info("hid-mouse: button %d %s", b + 1, pressed ? "pressed" : "released");
            }
        }
        m_prev_buttons = buttons;
    }

    if (scroll != 0) {
        log::debug("hid-mouse: scroll=%d", scroll);
    }
}

int32_t hid_mouse_handler::read_signed_field(const uint8_t* data,
                                              uint16_t bit_offset, uint16_t bit_size) {
    uint32_t byte_offset = bit_offset / 8;
    uint32_t bit_shift = bit_offset % 8;

    uint32_t raw = 0;
    uint32_t bytes_needed = (bit_shift + bit_size + 7) / 8;
    string::memcpy(&raw, data + byte_offset, bytes_needed > 4 ? 4 : bytes_needed);
    raw >>= bit_shift;
    raw &= (1u << bit_size) - 1;

    // Sign-extend
    int32_t shift = 32 - bit_size;
    return static_cast<int32_t>(raw << shift) >> shift;
}

uint32_t hid_mouse_handler::read_unsigned_field(const uint8_t* data,
                                                 uint16_t bit_offset, uint16_t bit_size) {
    uint32_t byte_offset = bit_offset / 8;
    uint32_t bit_shift = bit_offset % 8;

    uint32_t raw = 0;
    uint32_t bytes_needed = (bit_shift + bit_size + 7) / 8;
    string::memcpy(&raw, data + byte_offset, bytes_needed > 4 ? 4 : bytes_needed);
    raw >>= bit_shift;
    raw &= (1u << bit_size) - 1;

    return raw;
}

} // namespace usb::hid
