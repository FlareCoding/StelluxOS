#ifndef STLXDM_INPUT_HPP
#define STLXDM_INPUT_HPP

#include <cstdint>

class server;
struct screen;

/* Input routing: reads the kernel input devices, tracks the pointer
 * and keyboard focus, and feeds translated events to the server for
 * delivery. The cursor sprite is compose work and lives elsewhere. */
class input {
public:
    /* Opens /dev/input/kbd and /dev/input/mouse. Missing devices are
     * tolerated, the desktop just runs without that input kind. */
    int init(const screen* scr);
    void shutdown();

    int kbd_fd() const { return m_kbd_fd; }
    int mouse_fd() const { return m_mouse_fd; }

    /* Drains readable device events and routes them. */
    void pump_kbd(server& srv);
    void pump_mouse(server& srv);

    /* Nanoseconds until the next key repeat fires, or -1 when idle. */
    int64_t repeat_timeout_ns(uint64_t now_ns) const;

    /* Emits repeat events whose deadline has passed. */
    void pump_repeat(server& srv, uint64_t now_ns);

    int32_t ptr_x() const { return m_ptr_x; }
    int32_t ptr_y() const { return m_ptr_y; }

private:
    int m_kbd_fd = -1;
    int m_mouse_fd = -1;

    int32_t m_ptr_x = 0;
    int32_t m_ptr_y = 0;
    int32_t m_max_x = 0;
    int32_t m_max_y = 0;
    uint16_t m_buttons = 0;

    /* Held key state driving DM-side repeat */
    uint16_t m_held_usage = 0;
    uint8_t  m_held_modifiers = 0;
    uint64_t m_repeat_deadline_ns = 0;
};

#endif
