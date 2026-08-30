#include "input.hpp"
#include "server.hpp"

#include <stlx/input.h>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

int input::init(uint32_t screen_w, uint32_t screen_h,
                uint64_t repeat_delay_ns, uint64_t repeat_interval_ns) {
    m_max_x = static_cast<int32_t>(screen_w) - 1;
    m_max_y = static_cast<int32_t>(screen_h) - 1;
    m_ptr_x = m_max_x / 2;
    m_ptr_y = m_max_y / 2;
    m_repeat_delay_ns = repeat_delay_ns;
    m_repeat_interval_ns = repeat_interval_ns;

    m_kbd_fd = open("/dev/input/kbd", O_RDONLY | O_NONBLOCK);
    m_mouse_fd = open("/dev/input/mouse", O_RDONLY | O_NONBLOCK);

    return (m_kbd_fd >= 0 || m_mouse_fd >= 0) ? 0 : -1;
}

void input::shutdown() {
    if (m_kbd_fd >= 0) {
        close(m_kbd_fd);
        m_kbd_fd = -1;
    }
    if (m_mouse_fd >= 0) {
        close(m_mouse_fd);
        m_mouse_fd = -1;
    }
}

void input::pump_kbd(server& srv) {
    stlx_input_kbd_event_t ev;

    while (read(m_kbd_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        bool down = ev.action == STLX_INPUT_KBD_ACTION_DOWN;

        /* Modifier usages never repeat and carry no character, but a
         * held key must repeat with the modifiers as they are now */
        if (ev.usage >= 0xE0 && ev.usage <= 0xE7) {
            m_held_modifiers = ev.modifiers;
            srv.route_key(ev.usage, ev.modifiers, down, false);
            continue;
        }

        if (down) {
            m_held_usage = ev.usage;
            m_held_modifiers = ev.modifiers;
            m_repeat_deadline_ns = 0;
        } else if (ev.usage == m_held_usage) {
            m_held_usage = 0;
        }

        srv.route_key(ev.usage, ev.modifiers, down, false);
    }
}

void input::pump_mouse(server& srv) {
    stlx_input_mouse_event_t ev;

    while (read(m_mouse_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.flags & STLX_INPUT_MOUSE_FLAG_RELATIVE) {
            m_ptr_x += ev.x_value;
            m_ptr_y += ev.y_value;
        } else {
            m_ptr_x = ev.x_value;
            m_ptr_y = ev.y_value;
        }

        if (m_ptr_x < 0) m_ptr_x = 0;
        if (m_ptr_y < 0) m_ptr_y = 0;
        if (m_ptr_x > m_max_x) m_ptr_x = m_max_x;
        if (m_ptr_y > m_max_y) m_ptr_y = m_max_y;

        uint16_t changed = ev.buttons ^ m_buttons;
        m_buttons = ev.buttons;

        srv.route_pointer(m_ptr_x, m_ptr_y, ev.buttons, changed, ev.wheel);
    }
}

int64_t input::repeat_timeout_ns(uint64_t now_ns) const {
    if (m_held_usage == 0 || m_repeat_delay_ns == 0) {
        return -1;
    }

    uint64_t deadline = m_repeat_deadline_ns;
    if (deadline == 0) {
        return static_cast<int64_t>(m_repeat_delay_ns);
    }

    return deadline > now_ns ? static_cast<int64_t>(deadline - now_ns) : 0;
}

void input::pump_repeat(server& srv, uint64_t now_ns) {
    if (m_held_usage == 0 || m_repeat_delay_ns == 0) {
        return;
    }

    if (m_repeat_deadline_ns == 0) {
        m_repeat_deadline_ns = now_ns + m_repeat_delay_ns;
        return;
    }

    while (m_repeat_deadline_ns <= now_ns) {
        srv.route_key(m_held_usage, m_held_modifiers, true, true);
        m_repeat_deadline_ns += m_repeat_interval_ns;
    }
}
