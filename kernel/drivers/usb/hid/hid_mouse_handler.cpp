#include "drivers/usb/hid/hid_mouse_handler.h"
#include "drivers/usb/hid/hid_constants.h"
#include "drivers/input/input.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"

namespace usb::hid {

hid_mouse_handler::~hid_mouse_handler() {
    reset_state();
}

void hid_mouse_handler::reset_state() {
    if (m_button_fields) {
        heap::ufree(m_button_fields);
        m_button_fields = nullptr;
    }
    if (m_prev_buttons) {
        heap::ufree(m_prev_buttons);
        m_prev_buttons = nullptr;
    }

    m_x_field = nullptr;
    m_y_field = nullptr;
    m_wheel_field = nullptr;
    m_button_count = 0;
    m_report_id = 0;
    m_ready = false;
}

int32_t hid_mouse_handler::init(const report_layout& layout,
                                const input_report_info& report) {
    reset_state();

    auto gd = static_cast<uint16_t>(usage_page::generic_desktop);
    auto btn = static_cast<uint16_t>(usage_page::buttons);
    const field_info* fields = report_fields(layout, report);
    if (!fields || report.field_count == 0) {
        return -1;
    }

    uint16_t button_count = 0;
    for (uint16_t i = 0; i < report.field_count; i++) {
        const auto& field = fields[i];
        if (field.is_constant()) {
            continue;
        }

        if (!m_x_field &&
            field.usage_page == gd &&
            field.usage == static_cast<uint16_t>(generic_desktop_usage::x_axis)) {
            m_x_field = &field;
            continue;
        }
        if (!m_y_field &&
            field.usage_page == gd &&
            field.usage == static_cast<uint16_t>(generic_desktop_usage::y_axis)) {
            m_y_field = &field;
            continue;
        }
        if (!m_wheel_field &&
            field.usage_page == gd &&
            field.usage == static_cast<uint16_t>(generic_desktop_usage::wheel)) {
            m_wheel_field = &field;
            continue;
        }
        if (field.usage_page == btn && field.is_variable()) {
            button_count++;
        }
    }

    if (!m_x_field || !m_y_field) {
        reset_state();
        return -1;
    }

    if (button_count > 0) {
        m_button_fields = static_cast<const field_info**>(
            heap::ualloc(button_count * sizeof(field_info*)));
        m_prev_buttons = static_cast<uint8_t*>(
            heap::ualloc(button_count * sizeof(uint8_t)));
        if (!m_button_fields || !m_prev_buttons) {
            reset_state();
            return -1;
        }

        uint16_t button_index = 0;
        for (uint16_t i = 0; i < report.field_count; i++) {
            const auto& field = fields[i];
            if (!field.is_constant() &&
                field.usage_page == btn &&
                field.is_variable()) {
                m_button_fields[button_index++] = &field;
            }
        }
        string::memset(m_prev_buttons, 0, button_count * sizeof(uint8_t));
    }

    m_button_count = button_count;
    m_report_id = report.report_id;
    m_ready = true;

    log::info("hid-mouse: report=%u buttons=%u wheel=%s",
              m_report_id, m_button_count, m_wheel_field ? "yes" : "no");
    return 0;
}

void hid_mouse_handler::on_report(const uint8_t* data, uint32_t length) {
    if (!m_ready) {
        return;
    }

    int32_t dx = 0;
    int32_t dy = 0;
    int32_t scroll = 0;

    if (m_x_field) {
        dx = (m_x_field->logical_minimum < 0 || m_x_field->is_relative())
            ? read_field_signed(data, length, m_x_field->bit_offset, m_x_field->bit_size)
            : static_cast<int32_t>(read_field_unsigned(
                data, length, m_x_field->bit_offset, m_x_field->bit_size));
    }
    if (m_y_field) {
        dy = (m_y_field->logical_minimum < 0 || m_y_field->is_relative())
            ? read_field_signed(data, length, m_y_field->bit_offset, m_y_field->bit_size)
            : static_cast<int32_t>(read_field_unsigned(
                data, length, m_y_field->bit_offset, m_y_field->bit_size));
    }
    if (m_wheel_field) {
        scroll = (m_wheel_field->logical_minimum < 0 || m_wheel_field->is_relative())
            ? read_field_signed(data, length, m_wheel_field->bit_offset, m_wheel_field->bit_size)
            : static_cast<int32_t>(read_field_unsigned(
                data, length, m_wheel_field->bit_offset, m_wheel_field->bit_size));
    }

    uint16_t buttons = 0;
    bool buttons_changed = false;
    for (uint16_t i = 0; i < m_button_count; i++) {
        const auto* field = m_button_fields[i];
        bool pressed = read_field_unsigned(data, length, field->bit_offset, field->bit_size) != 0;
        if (pressed) {
            buttons |= static_cast<uint16_t>(1u << i);
        }
        bool was_pressed = m_prev_buttons[i] != 0;
        if (pressed != was_pressed) {
            buttons_changed = true;
        }
        m_prev_buttons[i] = pressed ? 1 : 0;
    }

    bool is_relative = m_x_field && (m_x_field->logical_minimum < 0 || m_x_field->is_relative());

    if (dx != 0 || dy != 0 || scroll != 0 || buttons_changed) {
        input::mouse_event evt{};
        evt.x_value = dx;
        evt.y_value = dy;
        evt.wheel = static_cast<int16_t>(scroll);
        evt.buttons = buttons;
        evt.flags = is_relative ? input::MOUSE_FLAG_RELATIVE : 0;
        input::push_mouse_event(evt);
    }
}

} // namespace usb::hid
