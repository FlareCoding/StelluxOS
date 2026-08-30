#include "decor.hpp"
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

/* Topmost window whose decorated bounds contain the point, walking
 * the scene top-down and reporting which zone was struck. */
dm_window* server::window_at(int32_t x, int32_t y, decor::zone* out_zone) {
    for (size_t i = m_zorder.size(); i-- > 0;) {
        dm_window* w = m_zorder[i];
        decor::zone z = decor::hit(*w, x, y);
        if (z != decor::zone::none) {
            if (out_zone) {
                *out_zone = z;
            }
            return w;
        }
    }

    if (out_zone) {
        *out_zone = decor::zone::none;
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

    dm_window* old = m_focus;
    m_focus = w;
    if (w) {
        rec.kind = SWP_EV_FOCUS_IN;
        send_event(w, rec);
    }

    if (old) {
        scene_damage_window(old);
    }
    if (w) {
        scene_damage_window(w);
    }
}

void server::forget_window(dm_window* w) {
    if (m_focus == w) m_focus = nullptr;
    if (m_hover == w) m_hover = nullptr;
    if (m_grab == w) m_grab = nullptr;
    if (m_grab_popup == w) m_grab_popup = nullptr;
    if (m_focus_restore == w) m_focus_restore = nullptr;
    if (m_drag == w) m_drag = nullptr;
    if (m_close_hover == w) m_close_hover = nullptr;
    if (m_close_press == w) m_close_press = nullptr;

    for (size_t i = 0; i < m_zorder.size(); i++) {
        if (m_zorder[i] == w) {
            m_zorder.erase(m_zorder.begin() + (long)i);
            break;
        }
    }
}

void server::route_key(uint16_t usage, uint8_t hid_modifiers, bool down,
                       bool repeat) {
    if (!m_focus) {
        return;
    }

    swp_event_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.modifiers = cook_modifiers(hid_modifiers);

    rec.kind = repeat ? SWP_EV_KEY_REPEAT
             : down   ? SWP_EV_KEY_DOWN
             :          SWP_EV_KEY_UP;
    rec.usage = usage;
    rec.ch = down ? cook_codepoint(usage, hid_modifiers) : 0;

    send_event(m_focus, rec);
}

void server::route_pointer(int32_t x, int32_t y, uint16_t buttons,
                           uint16_t changed, int16_t wheel) {
    bool press = changed != 0 && (buttons & changed) != 0;
    bool all_released = (buttons & 0x7) == 0;

    /* An active title drag owns the pointer until every button lifts */
    if (m_drag) {
        scene_damage_window(m_drag);
        m_drag->x = x - m_drag_dx;
        m_drag->y = y - m_drag_dy;
        scene_damage_window(m_drag);

        if (all_released) {
            m_drag = nullptr;
        }
        return;
    }

    /* A press outside a grabbing popup dismisses it, restores the
     * focus it displaced, and swallows the press */
    if (m_grab_popup && press) {
        dm_window* hit = window_at(x, y, nullptr);
        if (hit != m_grab_popup) {
            dm_window* popup = m_grab_popup;
            dm_window* restore = m_focus_restore;
            dm_client* owner = window_owner(popup);
            uint32_t parent_id = popup->parent_id;
            m_grab_popup = nullptr;
            m_focus_restore = nullptr;

            if (owner) {
                dm_window* parent = nullptr;
                for (auto& win : owner->windows) {
                    if (win->win_id == parent_id) {
                        parent = win.get();
                        break;
                    }
                }
                if (parent) {
                    swp_event_rec rec;
                    memset(&rec, 0, sizeof(rec));
                    rec.kind = SWP_EV_POPUP_DISMISSED;
                    send_event(parent, rec);
                }

                destroy_window_tree(*owner, popup->win_id);
            }

            set_focus(restore);
            return;
        }
    }

    decor::zone zone = decor::zone::none;
    dm_window* struck = m_grab ? m_grab : window_at(x, y, &zone);
    if (m_grab) {
        zone = decor::zone::content;
    }

    /* Close-control hover repaints the title bars it touches */
    dm_window* hover_close =
        zone == decor::zone::close ? struck : nullptr;
    if (hover_close != m_close_hover) {
        if (m_close_hover) {
            scene_damage_window(m_close_hover);
        }
        m_close_hover = hover_close;
        if (m_close_hover) {
            scene_damage_window(m_close_hover);
        }
    }

    /* Decoration interactions never reach the client */
    if (zone == decor::zone::title || zone == decor::zone::close) {
        if (press) {
            set_focus(struck);
            scene_raise(struck);

            if (zone == decor::zone::title) {
                m_drag = struck;
                m_drag_dx = x - struck->x;
                m_drag_dy = y - struck->y;
            } else {
                m_close_press = struck;
            }
        } else if (changed != 0 && m_close_press == struck &&
                   zone == decor::zone::close) {
            swp_event_rec rec;
            memset(&rec, 0, sizeof(rec));
            rec.kind = SWP_EV_CLOSE;
            send_event(struck, rec);
            m_close_press = nullptr;
        }
        return;
    }

    if (changed != 0 && all_released) {
        m_close_press = nullptr;
    }

    dm_window* target = zone == decor::zone::content ? struck : nullptr;

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
            scene_raise(target);
            m_grab = target;
        } else if (m_grab && all_released) {
            m_grab = nullptr;
        }

        send_event(target, rec);
    }
}
