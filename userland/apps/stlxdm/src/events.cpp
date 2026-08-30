#include "decor.hpp"
#include "server.hpp"

#include <stlx/input.h>

#include <cstring>

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

/* Content edges a resize drag moves */
constexpr uint8_t EDGE_L = 1u << 0;
constexpr uint8_t EDGE_R = 1u << 1;
constexpr uint8_t EDGE_T = 1u << 2;
constexpr uint8_t EDGE_B = 1u << 3;

/* Windows never shrink below this content size */
constexpr uint32_t MIN_CONTENT_W = 64;
constexpr uint32_t MIN_CONTENT_H = 48;

/* Wire modifier bits from the raw HID modifier byte */
static uint8_t cook_modifiers(uint8_t hid) {
    uint8_t out = 0;
    if (hid & HID_SHIFT) out |= 1u << 0;
    if (hid & HID_CTRL)  out |= 1u << 1;
    if (hid & HID_ALT)   out |= 1u << 2;
    if (hid & HID_GUI)   out |= 1u << 3;
    return out;
}

static uint8_t zone_edges(decor::zone z) {
    switch (z) {
    case decor::zone::resize_l:  return EDGE_L;
    case decor::zone::resize_r:  return EDGE_R;
    case decor::zone::resize_t:  return EDGE_T;
    case decor::zone::resize_b:  return EDGE_B;
    case decor::zone::resize_tl: return EDGE_T | EDGE_L;
    case decor::zone::resize_tr: return EDGE_T | EDGE_R;
    case decor::zone::resize_bl: return EDGE_B | EDGE_L;
    case decor::zone::resize_br: return EDGE_B | EDGE_R;
    default:                     return 0;
    }
}

static uint32_t zone_shape(decor::zone z) {
    switch (z) {
    case decor::zone::resize_l:
    case decor::zone::resize_r:  return SWP_CURSOR_RESIZE_H;
    case decor::zone::resize_t:
    case decor::zone::resize_b:  return SWP_CURSOR_RESIZE_V;
    case decor::zone::resize_tl:
    case decor::zone::resize_br: return SWP_CURSOR_RESIZE_NWSE;
    case decor::zone::resize_tr:
    case decor::zone::resize_bl: return SWP_CURSOR_RESIZE_NESW;
    default:                     return SWP_CURSOR_ARROW;
    }
}

/* US layout usage to codepoint, 0 for keys without a character */
static uint32_t cook_codepoint(uint16_t usage, uint8_t hid) {
    bool shift = (hid & HID_SHIFT) != 0;

    if (usage >= 0x04 && usage <= 0x1D) {
        uint32_t ch = 'a' + (usage - 0x04);
        return shift ? ch - 32 : ch;
    }

    if (usage >= 0x1E && usage <= 0x27) {
        uint32_t idx = usage - 0x1E;
        return static_cast<uint32_t>(shift ? DIGIT_SHIFT[idx] : DIGIT_PLAIN[idx]);
    }

    if (usage >= 0x2D && usage <= 0x38) {
        uint32_t idx = usage - 0x2D;
        char ch = shift ? PUNCT_SHIFT[idx] : PUNCT_PLAIN[idx];
        return static_cast<uint32_t>(ch);
    }

    switch (usage) {
    case 0x28: return '\r';
    case 0x2A: return '\b';
    case 0x2B: return '\t';
    case 0x2C: return ' ';
    default:   return 0;
    }
}

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

/* Events stage on the window and leave as one message per wakeup */
void server::send_event(dm_window* w, const swp_event_rec& rec) {
    if (w->ev_batch_count == DM_EV_BATCH_MAX) {
        dm_client* c = window_owner(w);
        if (!c) {
            return;
        }
        flush_window_events(*c, *w);
    }

    w->ev_batch[w->ev_batch_count++] = rec;
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
    if (m_resize == w) {
        damage_outline();
        m_resize = nullptr;
    }

    for (size_t i = 0; i < m_zorder.size(); i++) {
        if (m_zorder[i] == w) {
            m_zorder.erase(m_zorder.begin() + static_cast<long>(i));
            break;
        }
    }
}

void server::route_key(uint16_t usage, uint8_t hid_modifiers, bool down,
                       bool repeat) {
    /* An active overlay swallows the keyboard, escape backs out */
    if (m_power.active()) {
        if (down && !repeat && usage == 0x29) {
            m_power.dismiss();
        }
        return;
    }

    /* Config hotkeys fire on press and never reach the focus */
    if (down && !repeat) {
        uint8_t mods = cook_modifiers(hid_modifiers);
        for (const dm_hotkey& hk : m_hotkeys) {
            if (usage == hk.usage && (mods & hk.mods) == hk.mods) {
                spawn_shortcut(hk.path);
                return;
            }
        }
    }

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

/* Damages the sprite's old and new bounds so both repaint */
void server::move_cursor(int32_t x, int32_t y, uint32_t shape) {
    if (x == m_cursor_x && y == m_cursor_y && shape == m_cursor_shape) {
        return;
    }

    int32_t bx, by, bw, bh;
    m_cursor.bounds(m_cursor_shape, m_cursor_x, m_cursor_y,
                    &bx, &by, &bw, &bh);
    m_damage.add(bx, by, bw, bh);
    m_cursor.bounds(shape, x, y, &bx, &by, &bw, &bh);
    m_damage.add(bx, by, bw, bh);

    m_cursor_x = x;
    m_cursor_y = y;
    m_cursor_shape = shape;
}

void server::damage_outline() {
    const damage_list::rect& r = m_outline;
    if (r.w <= 0 || r.h <= 0) {
        return;
    }

    m_damage.add(r.x, r.y, r.w, decor::OUTLINE_T);
    m_damage.add(r.x, r.y + r.h - decor::OUTLINE_T, r.w, decor::OUTLINE_T);
    m_damage.add(r.x, r.y, decor::OUTLINE_T, r.h);
    m_damage.add(r.x + r.w - decor::OUTLINE_T, r.y, decor::OUTLINE_T, r.h);
}

void server::begin_resize(dm_window* w, decor::zone z, uint32_t shape) {
    const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];

    m_resize = w;
    m_resize_edges = zone_edges(z);
    m_resize_shape = shape;
    m_resize_anchor_x = (m_resize_edges & EDGE_L)
                      ? w->x + static_cast<int32_t>(b.width) : w->x;
    m_resize_anchor_y = (m_resize_edges & EDGE_T)
                      ? w->y + static_cast<int32_t>(b.height) : w->y;

    m_outline = decor::bounds(*w);
    damage_outline();
}

/* Recomputes the drag target from the pointer, keeping the opposite
 * edges anchored, and moves the rubber band to it */
void server::update_resize(int32_t px, int32_t py) {
    dm_window* w = m_resize;
    const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];

    uint32_t min_w = w->min_w > MIN_CONTENT_W ? w->min_w : MIN_CONTENT_W;
    uint32_t min_h = w->min_h > MIN_CONTENT_H ? w->min_h : MIN_CONTENT_H;

    uint32_t tw = b.width;
    uint32_t th = b.height;
    int32_t tx = w->x;
    int32_t ty = w->y;

    if (m_resize_edges & EDGE_L) {
        int32_t raw = m_resize_anchor_x - px;
        tw = raw < static_cast<int32_t>(min_w) ? min_w : static_cast<uint32_t>(raw);
        if (w->max_w != 0 && tw > w->max_w) {
            tw = w->max_w;
        }
        tx = m_resize_anchor_x - static_cast<int32_t>(tw);
    } else if (m_resize_edges & EDGE_R) {
        int32_t raw = px - w->x;
        tw = raw < static_cast<int32_t>(min_w) ? min_w : static_cast<uint32_t>(raw);
        if (w->max_w != 0 && tw > w->max_w) {
            tw = w->max_w;
        }
    }

    if (m_resize_edges & EDGE_T) {
        int32_t raw = m_resize_anchor_y - py;
        th = raw < static_cast<int32_t>(min_h) ? min_h : static_cast<uint32_t>(raw);
        if (w->max_h != 0 && th > w->max_h) {
            th = w->max_h;
        }
        ty = m_resize_anchor_y - static_cast<int32_t>(th);
    } else if (m_resize_edges & EDGE_B) {
        int32_t raw = py - w->y;
        th = raw < static_cast<int32_t>(min_h) ? min_h : static_cast<uint32_t>(raw);
        if (w->max_h != 0 && th > w->max_h) {
            th = w->max_h;
        }
    }

    w->target_w = tw;
    w->target_h = th;
    w->target_x = tx;
    w->target_y = ty;

    damage_outline();
    m_outline = decor::frame_rect(*w, tx, ty, static_cast<int32_t>(tw), static_cast<int32_t>(th));
    damage_outline();
}

void server::route_pointer(int32_t x, int32_t y, uint16_t buttons,
                           uint16_t changed, int16_t wheel) {
    bool press = changed != 0 && (buttons & changed) != 0;
    bool all_released = (buttons & 0x7) == 0;

    /* An active overlay owns the pointer entirely. Hover and hold
     * transitions repaint the orb boxes. */
    if (m_power.active()) {
        move_cursor(x, y, SWP_CURSOR_ARROW);

        int32_t old_hover = m_power.hover_choice();
        m_power.on_motion(x, y);
        if ((changed & 1) != 0) {
            if ((buttons & 1) != 0) {
                m_power.on_press(x, y);
            } else {
                m_power.on_release();
            }
        }

        if (m_power.hover_choice() != old_hover || (changed & 1) != 0) {
            for (int32_t i = 0; i < 2; i++) {
                damage_list::rect ob = m_power.orb_box(i);
                m_damage.add(ob.x, ob.y, ob.w, ob.h);
            }
        }
        return;
    }

    /* Star hover tracking while the overlay is closed, a change
     * repaints the sprite's glow */
    bool star_was = m_power.star_hover();
    m_power.on_motion(x, y);
    if (m_power.star_hover() != star_was) {
        m_damage.add_full();
    }

    /* A press on the star opens the overlay above everything */
    if (press && m_power.star_hit(x, y)) {
        m_power.open();
        m_damage.add_full();
        return;
    }

    /* An active resize drag owns the pointer until every button lifts,
     * and the final target flushes as one last configure */
    if (m_resize) {
        move_cursor(x, y, m_resize_shape);
        update_resize(x, y);

        if (all_released) {
            damage_outline();
            m_resize = nullptr;
        }
        return;
    }

    /* An active title drag owns the pointer until every button lifts.
     * The title bar stays reachable between the bar and the dock. */
    if (m_drag) {
        move_cursor(x, y, m_cursor_shape);

        int32_t want_y = y - m_drag_dy;
        int32_t min_y = dm_panels::BAR_H + decor::TITLE_H;
        if (want_y < min_y) {
            want_y = min_y;
        }
        if (want_y > m_panels.dock_y()) {
            want_y = m_panels.dock_y();
        }

        scene_damage_window(m_drag);
        int32_t dx = x - m_drag_dx - m_drag->x;
        int32_t dy = want_y - m_drag->y;
        m_drag->x += dx;
        m_drag->y += dy;
        scene_damage_window(m_drag);

        /* Positions promised to unacked configures move with the drag,
         * or a late resize ack would snap the window back */
        for (uint32_t i = 0; i < m_drag->sent_conf_count; i++) {
            m_drag->sent_confs[i].x += dx;
            m_drag->sent_confs[i].y += dy;
        }
        if (m_drag->target_w != 0) {
            m_drag->target_x += dx;
            m_drag->target_y += dy;
        }

        /* Dropping the window sheds the drag glow */
        if (all_released) {
            dm_window* dropped = m_drag;
            m_drag = nullptr;
            scene_damage_window(dropped);
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

    /* The shape follows the content window's request, and the frame
     * band advertises the resize direction it would grab */
    uint8_t resize_edges = zone_edges(zone);
    uint32_t shape = zone == decor::zone::content && struck
                   ? struck->cursor
                   : resize_edges != 0 ? zone_shape(zone)
                                       : SWP_CURSOR_ARROW;
    move_cursor(x, y, shape);

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
    if (zone == decor::zone::title || zone == decor::zone::close ||
        resize_edges != 0) {
        if (press) {
            set_focus(struck);
            scene_raise(struck);

            if (resize_edges != 0) {
                begin_resize(struck, zone, shape);
            } else if (zone == decor::zone::title) {
                m_drag = struck;
                m_drag_dx = x - struck->x;
                m_drag_dy = y - struck->y;

                /* The drag glow appears on grab */
                scene_damage_window(struck);
            } else {
                m_close_press = struck;
                scene_damage_window(struck);
            }
        } else if (changed != 0 && m_close_press == struck &&
                   zone == decor::zone::close) {
            swp_event_rec rec;
            memset(&rec, 0, sizeof(rec));
            rec.kind = SWP_EV_CLOSE;
            send_event(struck, rec);
            m_close_press = nullptr;
            scene_damage_window(struck);
        }
        return;
    }

    if (changed != 0 && all_released && m_close_press) {
        scene_damage_window(m_close_press);
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
        /* Uncovered band area belongs to the panels, and a motion
         * elsewhere clears any panel hover */
        if (!struck && m_panels.contains(x, y)) {
            if (changed == 0 && wheel == 0) {
                m_panels.pointer_move(x, y);
            }

            for (uint8_t btn = 0; btn < 3; btn++) {
                uint16_t bit = static_cast<uint16_t>(1u << btn);
                if (changed & bit) {
                    m_panels.pointer_button(x, y, btn,
                                            (buttons & bit) != 0);
                }
            }
        } else {
            m_panels.pointer_move(-1, -1);
        }

        return;
    }

    m_panels.pointer_move(-1, -1);

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
        uint16_t bit = static_cast<uint16_t>(1u << btn);
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
