#ifndef STLXDM_INPUT_HPP
#define STLXDM_INPUT_HPP

#include "layout.hpp"

#include <cstdint>

class server;

/* Input routing: reads the kernel input devices, tracks the pointer
 * and keyboard focus, and feeds translated events to the server for
 * delivery. The server shows the pointer. */
class input {
public:
    /* Opens /dev/input/kbd and /dev/input/mouse. The pointer starts at
     * the main output's center and moves over the places of `desktop`,
     * which must hold a member and outlive the input. Missing devices are
     * tolerated, the desktop just runs without that input kind. A repeat
     * delay of zero disables key repeat. */
    int init(const layout* desktop, uint64_t repeat_delay_ns, uint64_t repeat_interval_ns);

    /* Puts the pointer at `point`, which must lie on a place. */
    void set_pointer(desktop_point point) { m_pointer = point; }

    void shutdown();

    int kbd_fd() const { return m_kbd_fd; }
    int mouse_fd() const { return m_mouse_fd; }

    /* Drains readable device events and routes them. */
    void pump_kbd(server& srv);
    void pump_mouse(server& srv);

    /* Nanoseconds until the next key repeat fires, or -1 when idle. */
    int64_t repeat_timeout_ns(uint64_t now_ns) const;

    /* Applies reloaded repeat rates, a delay of zero disables. */
    void set_repeat_rates(uint64_t delay_ns, uint64_t interval_ns) {
        m_repeat_delay_ns = delay_ns;
        m_repeat_interval_ns = interval_ns;
    }

    /* Emits repeat events whose deadline has passed. */
    void pump_repeat(server& srv, uint64_t now_ns);

    desktop_point pointer() const { return m_pointer; }
    uint16_t buttons() const { return m_buttons; }

private:
    int m_kbd_fd = -1;
    int m_mouse_fd = -1;

    const layout* m_desktop = nullptr;
    desktop_point m_pointer;
    uint16_t m_buttons = 0;

    /* Held key state driving DM-side repeat, rates from the config */
    uint64_t m_repeat_delay_ns = 0;
    uint64_t m_repeat_interval_ns = 0;
    uint16_t m_held_usage = 0;
    uint8_t  m_held_modifiers = 0;
    uint64_t m_repeat_deadline_ns = 0;
};

#endif
