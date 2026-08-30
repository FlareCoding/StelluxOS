#include "server.hpp"

#include <stlx/input.h>

#include <cstring>

namespace {

constexpr uint8_t HID_SHIFT =
    STLX_INPUT_MOD_LSHIFT | STLX_INPUT_MOD_RSHIFT;
constexpr uint8_t HID_CTRL =
    STLX_INPUT_MOD_LCTRL | STLX_INPUT_MOD_RCTRL;
constexpr uint8_t HID_ALT =
    STLX_INPUT_MOD_LALT | STLX_INPUT_MOD_RALT;
constexpr uint8_t HID_GUI =
    STLX_INPUT_MOD_LGUI | STLX_INPUT_MOD_RGUI;

constexpr char DIGIT_PLAIN[] = "1234567890";
constexpr char DIGIT_SHIFT[] = "!@#$%^&*()";
constexpr char PUNCT_PLAIN[] = {
    '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/'
};
constexpr char PUNCT_SHIFT[] = {
    '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?'
};

/* Wire modifier bits from the raw HID modifier byte */
uint8_t cook_modifiers(uint8_t hid) {
    uint8_t out = 0;
    if (hid & HID_SHIFT) out |= 1u << 0;
    if (hid & HID_CTRL)  out |= 1u << 1;
    if (hid & HID_ALT)   out |= 1u << 2;
    if (hid & HID_GUI)   out |= 1u << 3;
    return out;
}

/* US layout usage to codepoint, 0 for keys without a character */
uint32_t cook_codepoint(uint16_t usage, uint8_t hid) {
    bool shift = (hid & HID_SHIFT) != 0;

    if (usage >= 0x04 && usage <= 0x1D) {
        uint32_t ch = 'a' + (usage - 0x04);
        return shift ? ch - 32 : ch;
    }

    if (usage >= 0x1E && usage <= 0x27) {
        uint32_t idx = usage - 0x1E;
        return (uint32_t)(shift ? DIGIT_SHIFT[idx] : DIGIT_PLAIN[idx]);
    }

    if (usage >= 0x2D && usage <= 0x38) {
        uint32_t idx = usage - 0x2D;
        char ch = shift ? PUNCT_SHIFT[idx] : PUNCT_PLAIN[idx];
        return (uint32_t)ch;
    }

    switch (usage) {
    case 0x28: return '\r';
    case 0x2B: return '\t';
    case 0x2C: return ' ';
    default:   return 0;
    }
}

} // namespace

dm_client* server::window_owner(const dm_window* w) {
    for (auto& c : m_clients) {
        for (auto& win : c->windows) {
            if (win.get() == w) {
                return c.get();
            }
        }
    }

    return nullptr;
}

/* Topmost mapped window under the point: newest client first, popups
 * before their parents within a client. The scene layer replaces this
 * with real z-order. */
dm_window* server::window_at(int32_t x, int32_t y) {
    for (size_t ci = m_clients.size(); ci-- > 0;) {
        auto& wins = m_clients[ci]->windows;
        for (size_t wi = wins.size(); wi-- > 0;) {
            dm_window* w = wins[wi].get();
            if (!w->mapped || w->current < 0) {
                continue;
            }

            const dm_buffer& b = w->buffers[(size_t)w->current];
            if (x >= w->x && y >= w->y &&
                x < w->x + (int32_t)b.width &&
                y < w->y + (int32_t)b.height) {
                return w;
            }
        }
    }

    return nullptr;
}

void server::send_event(dm_window* w, const swp_event_rec& rec) {
    dm_client* c = window_owner(w);
    if (!c) {
        return;
    }

    struct {
        swp_event_prefix prefix;
        swp_event_rec rec;
    } msg = { { w->win_id, 1 }, rec };

    send_to(*c, SWP_MSG_EVENT, &msg, sizeof(msg));
}

void server::set_focus(dm_window* w) {
    if (m_focus == w) {
        return;
    }

    swp_event_rec rec;
    memset(&rec, 0, sizeof(rec));

    if (m_focus) {
        rec.kind = SWP_EV_FOCUS_OUT;
        send_event(m_focus, rec);
    }

    m_focus = w;
    if (w) {
        rec.kind = SWP_EV_FOCUS_IN;
        send_event(w, rec);
    }
}

void server::forget_window(dm_window* w) {
    if (m_focus == w) m_focus = nullptr;
    if (m_hover == w) m_hover = nullptr;
    if (m_grab == w) m_grab = nullptr;
}

void server::route_key(uint16_t usage, uint8_t hid_modifiers, bool down,
                       bool repeat) {
    if (!m_focus) {
        return;
    }

    swp_event_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.modifiers = cook_modifiers(hid_modifiers);

    /* Ctrl+Alt+Q asks the focused window to close, standing in for the
     * decoration close button until server chrome lands */
    constexpr uint16_t USAGE_Q = 0x14;
    if (down && !repeat && usage == USAGE_Q &&
        (rec.modifiers & 0x6) == 0x6) {
        rec.kind = SWP_EV_CLOSE;
        rec.modifiers = 0;
        send_event(m_focus, rec);
        return;
    }

    rec.kind = repeat ? SWP_EV_KEY_REPEAT
             : down   ? SWP_EV_KEY_DOWN
             :          SWP_EV_KEY_UP;
    rec.usage = usage;
    rec.ch = down ? cook_codepoint(usage, hid_modifiers) : 0;

    send_event(m_focus, rec);
}

void server::route_pointer(int32_t x, int32_t y, uint16_t buttons,
                           uint16_t changed, int16_t wheel) {
    /* A window holding the implicit grab owns motion until release */
    dm_window* target = m_grab ? m_grab : window_at(x, y);

    if (!m_grab && target != m_hover) {
        swp_event_rec rec;
        memset(&rec, 0, sizeof(rec));

        if (m_hover) {
            rec.kind = SWP_EV_LEAVE;
            send_event(m_hover, rec);
        }
        m_hover = target;
        if (target) {
            rec.kind = SWP_EV_ENTER;
            rec.x = x - target->x;
            rec.y = y - target->y;
            send_event(target, rec);
        }
    }

    if (!target) {
        return;
    }

    swp_event_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.x = x - target->x;
    rec.y = y - target->y;

    if (changed == 0 && wheel == 0) {
        rec.kind = SWP_EV_MOTION;
        send_event(target, rec);
        return;
    }

    if (wheel != 0) {
        rec.kind = SWP_EV_SCROLL;
        rec.scroll = wheel;
        send_event(target, rec);
    }

    for (uint8_t btn = 0; btn < 3; btn++) {
        uint16_t bit = (uint16_t)(1u << btn);
        if (!(changed & bit)) {
            continue;
        }

        bool down = (buttons & bit) != 0;
        rec.kind = down ? SWP_EV_BUTTON_DOWN : SWP_EV_BUTTON_UP;
        rec.button = btn;
        rec.scroll = 0;

        if (down) {
            set_focus(target);
            m_grab = target;
        } else if (m_grab && (buttons & 0x7) == 0) {
            m_grab = nullptr;
        }

        send_event(target, rec);
    }
}
