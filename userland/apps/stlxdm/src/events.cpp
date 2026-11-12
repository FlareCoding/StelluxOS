#include "decor.hpp"
#include "server.hpp"
#include "shell/chord.hpp"

#include <stlx/input.h>

#include <cstdio>
#include <cstring>
#include <ctime>

constexpr uint8_t HID_SHIFT =
    STLX_INPUT_MOD_LSHIFT | STLX_INPUT_MOD_RSHIFT;
constexpr uint8_t HID_CTRL =
    STLX_INPUT_MOD_LCTRL | STLX_INPUT_MOD_RCTRL;
constexpr uint8_t HID_ALT =
    STLX_INPUT_MOD_LALT | STLX_INPUT_MOD_RALT;
constexpr uint8_t HID_GUI =
    STLX_INPUT_MOD_LGUI | STLX_INPUT_MOD_RGUI;

constexpr uint16_t HID_USAGE_ESCAPE = 0x29;
constexpr uint16_t HID_USAGE_SPACE = 0x2C;
constexpr uint16_t HID_USAGE_ENTER = 0x28;
constexpr uint16_t HID_USAGE_KEYPAD_ENTER = 0x58;
constexpr uint16_t HID_USAGE_R = 0x15;
constexpr uint16_t HID_USAGE_S = 0x16;
constexpr uint16_t HID_USAGE_W = 0x1A;
constexpr uint16_t HID_USAGE_DELETE = 0x4C;

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

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

/* Wire modifier bits from the raw HID modifier byte */
static uint8_t cook_modifiers(uint8_t hid) {
    uint8_t out = 0;
    if (hid & HID_SHIFT) {
        out |= CHORD_MOD_SHIFT;
    }

    if (hid & HID_CTRL) {
        out |= CHORD_MOD_CTRL;
    }

    if (hid & HID_ALT) {
        out |= CHORD_MOD_ALT;
    }

    if (hid & HID_GUI) {
        out |= CHORD_MOD_SUPER;
    }

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

/* Gives the panels a motion, or each button that changed */
static void feed_panels(dm_panels& panels, int32_t x, int32_t y, uint16_t buttons, uint16_t changed, int16_t wheel) {
    if (changed == 0 && wheel == 0) {
        panels.pointer_move(x, y);
    }

    for (uint8_t btn = 0; btn < DM_BUTTONS; btn++) {
        uint16_t bit = static_cast<uint16_t>(1u << btn);
        if (changed & bit) {
            panels.pointer_button(x, y, btn, (buttons & bit) != 0);
        }
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

/* The window a popup opened over, or null for a toplevel */
dm_window* server::popup_parent(const dm_window& w) {
    dm_client* owner = w.parent_id != 0 ? window_owner(&w) : nullptr;
    if (!owner) {
        return nullptr;
    }

    for (auto& win : owner->windows) {
        if (win->win_id == w.parent_id) {
            return win.get();
        }
    }

    return nullptr;
}

/* Topmost window whose decorated bounds contain the point, walking the scene top-down and reporting which zone
 * was struck. None where an animating window lies above the scene. */
dm_window* server::window_at(int32_t x, int32_t y, decor::zone* out_zone) {
    for (const window_animation& a : m_animations) {
        const damage_list::rect& frame = a.rect;
        bool is_inside = x >= frame.x && x < frame.x + frame.w && y >= frame.y && y < frame.y + frame.h;
        if (is_inside) {
            if (out_zone) {
                *out_zone = decor::zone::none;
            }

            return nullptr;
        }
    }

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

/* A popup closing with the focus hands it to the menu it opened from, which takes back the grab when it grabs, else
 * returns it to the window its chain of menus took it from, else to the window it opened over */
void server::forget_window(dm_window* w) {
    dm_window* parent = popup_parent(*w);
    bool is_parent_popup = parent && parent->parent_id != 0;
    bool hands_grab = m_grab_popup == w && is_parent_popup && (parent->popup_flags & SWP_PF_GRAB) != 0;

    dm_window* focus_after = nullptr;
    if (m_focus == w && parent) {
        focus_after = is_parent_popup || !m_focus_restore ? parent : m_focus_restore;
    }

    if (m_focus == w) m_focus = nullptr;
    if (m_hover == w) m_hover = nullptr;
    if (m_grab == w) m_grab = nullptr;
    if (m_grab_popup == w) m_grab_popup = nullptr;
    if (m_focus_restore == w) m_focus_restore = nullptr;
    if (m_drag == w) m_drag = nullptr;

    if (m_control_hover.window == w) {
        m_control_hover = {};
    }

    if (m_control_press.window == w) {
        m_control_press = {};
    }

    if (m_title_press == w) {
        m_title_press = nullptr;
    }

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

    forget_animation(w);
    forget_drag_window(w);

    if (hands_grab) {
        m_grab_popup = parent;
    }

    if (focus_after) {
        set_focus(focus_after);
    }
}

/* The window the popup opened over hears it dismissed. `forget_window()` gives the focus back if the popup still holds
 * it. */
void server::dismiss_grab_popup() {
    dm_window* popup = m_grab_popup;
    dm_client* owner = window_owner(popup);
    if (!owner) {
        return;
    }

    dm_window* parent = find_window(*owner, popup->parent_id);
    if (parent) {
        swp_event_rec rec;
        memset(&rec, 0, sizeof(rec));
        rec.kind = SWP_EV_POPUP_DISMISSED;
        send_event(parent, rec);
    }

    dismiss_window_tree(*owner, popup->win_id);
}

void server::route_key(uint16_t usage, uint8_t hid_modifiers, bool down,
                       bool repeat) {
    if (down && !repeat) {
        m_last_press_ns = now_ns();
    }

    bool is_escape_press = down && !repeat && usage == HID_USAGE_ESCAPE;
    uint8_t mods = cook_modifiers(hid_modifiers);
    m_modifiers = mods;

    /* An active overlay swallows the keyboard, escape backs out */
    if (m_power->active()) {
        if (is_escape_press) {
            m_power->dismiss();
        }
        return;
    }

    if (m_capture.active()) {
        capture_key(usage, down, repeat);
        return;
    }

    if (m_preview.active()) {
        if (is_escape_press) {
            close_preview();
        } else if (down && !repeat && usage == HID_USAGE_DELETE) {
            discard_preview();
        }

        return;
    }

    if (route_drag_key(usage, down, repeat)) {
        return;
    }

    /* Config hotkeys fire on press with exactly their modifiers held, and never reach a popover or the focus */
    if (down && !repeat) {
        for (const dm_hotkey& hk : m_hotkeys) {
            if (usage == hk.usage && mods == hk.mods) {
                run_shortcut(hk);
                return;
            }
        }
    }

    /* A popover that takes the keyboard sees every other key, and an Esc it leaves closes it unless pinned */
    uint32_t ch = down ? cook_codepoint(usage, hid_modifiers) : 0;
    bool is_popover_keyboard = m_panels->popover_takes_keys();
    if (is_popover_keyboard && m_panels->popover_key({ usage, mods, ch }, down)) {
        return;
    }

    if (is_escape_press && m_panels->escape_closes_popover()) {
        m_panels->close_popover();
        return;
    }

    if (is_popover_keyboard || !m_focus) {
        return;
    }

    swp_event_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.modifiers = mods;

    rec.kind = repeat ? SWP_EV_KEY_REPEAT
             : down   ? SWP_EV_KEY_DOWN
             :          SWP_EV_KEY_UP;
    rec.usage = usage;
    rec.ch = ch;

    send_event(m_focus, rec);
}

/* The mode a key chooses in the capture bar: R a region, W a window, S a whole output */
static bool mode_of_key(uint16_t usage, capture_mode* mode) {
    switch (usage) {
    case HID_USAGE_R:
        *mode = capture_mode::region;
        return true;
    case HID_USAGE_W:
        *mode = capture_mode::window;
        return true;
    case HID_USAGE_S:
        *mode = capture_mode::screen;
        return true;
    default:
        return false;
    }
}

/* The capture overlay takes every key: Esc cancels, Space moves the region or switches between a region and windows,
 * Shift and Alt reshape the region and the window's chip at once, and with the bar R, W and S choose a mode and Enter
 * takes what it aims at */
void server::capture_key(uint16_t usage, bool down, bool repeat) {
    if (repeat) {
        return;
    }

    if (usage == HID_USAGE_ESCAPE && down) {
        close_capture();
        return;
    }

    capture_target target;
    bool is_enter = usage == HID_USAGE_ENTER || usage == HID_USAGE_KEYPAD_ENTER;
    if (down && is_enter && m_capture.has_bar() && m_capture.take(&target)) {
        capture_region(target);
        return;
    }

    capture_mode mode = capture_mode::region;
    if (down && m_capture.has_bar() && mode_of_key(usage, &mode)) {
        m_capture.choose(mode, m_damage);
    } else if (usage == HID_USAGE_SPACE && down) {
        m_capture.press_space(now_ns(), m_damage);
    } else if (usage == HID_USAGE_SPACE) {
        m_capture.release_space();
    }

    move_cursor(m_cursor_x, m_cursor_y, m_capture.cursor_shape());
    m_capture.on_motion(m_cursor_x, m_cursor_y, m_modifiers, m_damage);
}

/* The capture overlay takes the pointer whole, and a primary release takes what a drag, a click or the bar's Capture
 * names, or closes for the bar's close button */
void server::capture_pointer(int32_t x, int32_t y, uint16_t buttons, uint16_t changed) {
    move_cursor(x, y, m_capture.cursor_shape());
    m_capture.on_motion(x, y, m_modifiers, m_damage);
    if ((changed & 1) == 0) {
        return;
    }

    if ((buttons & 1) != 0) {
        m_capture.on_press(x, y, m_modifiers, m_damage);
        return;
    }

    capture_target target;
    capture_result result = m_capture.on_release(&target, m_damage);
    if (result == capture_result::take) {
        capture_region(target);
    } else if (result == capture_result::close) {
        close_capture();
    }
}

/* A thumbnail lights what the pointer is on, and a primary release over what its press took acts */
void server::thumbnail_pointer(int32_t x, int32_t y, uint16_t buttons, uint16_t changed) {
    uint64_t now = now_ns();
    if ((changed & 1) == 0) {
        m_thumbnails.pointer_move(x, y, now, m_damage);
        return;
    }

    if ((buttons & 1) != 0) {
        m_thumbnails.press(x, y, now, m_damage);
        return;
    }

    thumbnail_click click = m_thumbnails.release(x, y, now, m_damage);
    if (click.action == thumbnail_action::preview) {
        open_preview(click.job);
    } else if (click.action == thumbnail_action::retry) {
        retry_capture(click.job);
    } else if (click.action == thumbnail_action::discard) {
        discard_capture(click);
        m_thumbnails.pointer_move(x, y, now, m_damage);
    }
}

static bool rects_overlap(const damage_list::rect& a, const damage_list::rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

void server::show_cursor() {
    damage_list::rect bounds = cursor_bounds();
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        dm_output& out = m_outputs[output];
        if (!out.pres) {
            continue;
        }

        out.cursor_picture = nullptr;
        out.cursor_move_pending = false;
        out.cursor_overlaps = rects_overlap(bounds, out.place);
        if (!out.cursor_overlaps) {
            out.pres->hide_cursor();
            out.cursor_in_frames = false;
            continue;
        }

        out.cursor_in_frames = !show_hardware_cursor(out);
        printf("stlxdm: pointer %s of output %u\r\n",
               out.cursor_in_frames ? "drawn into frames" : "on the display's hardware cursor", output);
    }

    set_drag_carrier(drag_carrier_shown());
    damage_cursor();
}

void server::flush_cursor(uint32_t output) {
    dm_output& out = m_outputs[output];
    if (!out.cursor_move_pending) {
        return;
    }

    out.cursor_move_pending = false;

    if (!out.pres->move_cursor(m_cursor_x - out.place.x, m_cursor_y - out.place.y)) {
        out.pres->hide_cursor();
        out.cursor_picture = nullptr;
        out.cursor_in_frames = true;
        set_drag_carrier(drag_carrier_shown());
        damage_cursor();
    }
}

/* An output whose cursor call failed draws the pointer into its frames
 * until a shape change or `follow_outputs()` tries the hardware cursor */
void server::move_cursor(int32_t x, int32_t y, uint32_t shape) {
    if (x == m_cursor_x && y == m_cursor_y && shape == m_cursor_shape) {
        return;
    }

    bool reshaped = shape != m_cursor_shape;
    damage_cursor();

    m_cursor_x = x;
    m_cursor_y = y;
    m_cursor_shape = shape;

    damage_list::rect bounds = cursor_bounds();
    for (dm_output& out : m_outputs) {
        if (!out.pres) {
            continue;
        }

        bool overlaps = rects_overlap(bounds, out.place);
        bool entered = overlaps && !out.cursor_overlaps;
        bool left = !overlaps && out.cursor_overlaps;
        out.cursor_overlaps = overlaps;

        if (left) {
            if (out.cursor_picture) {
                out.pres->hide_cursor();
            }

            out.cursor_picture = nullptr;
            out.cursor_move_pending = false;
        } else if (overlaps && (reshaped || (entered && !out.cursor_in_frames))) {
            out.cursor_in_frames = !show_hardware_cursor(out);
            out.cursor_move_pending = false;
        } else if (overlaps && out.cursor_picture) {
            out.cursor_move_pending = true;
        }
    }

    damage_cursor();
}

/* Shows the pointer's shape on `out`'s hardware cursor, sending its picture unless it shows already, or hides the
 * cursor and returns false. While a drag's picture follows the pointer, the cursor shows both or neither. */
bool server::show_hardware_cursor(dm_output& out) {
    if (drag_follows_pointer()) {
        return show_drag_cursor(out);
    }

    presenter::cursor_picture picture;
    bool has_picture = m_cursor.picture(m_cursor_shape, picture);
    bool showing = has_picture && picture.pixels == out.cursor_picture;

    bool shown = has_picture && out.pres->move_cursor(m_cursor_x - out.place.x, m_cursor_y - out.place.y);
    if (shown && !showing) {
        shown = out.pres->set_cursor(picture);
    }

    if (!shown) {
        out.pres->hide_cursor();
    }

    out.cursor_picture = shown ? picture.pixels : nullptr;

    return shown;
}

damage_list::rect server::cursor_bounds() const {
    damage_list::rect r;
    m_cursor.bounds(m_cursor_shape, m_cursor_x, m_cursor_y, &r.x, &r.y, &r.w, &r.h);
    return r;
}

/* Marks the sprite's bounds at the pointer's position and shape for the
 * next frame of each output drawing it to repaint */
void server::damage_cursor() {
    damage_list::rect r = cursor_bounds();
    for (const dm_output& out : m_outputs) {
        if (out.pres && out.cursor_in_frames && rects_overlap(r, out.place)) {
            m_damage.add(r.x, r.y, r.w, r.h);
            return;
        }
    }
}

void server::damage_outline() {
    if (m_outline.w <= 0 || m_outline.h <= 0) {
        return;
    }

    damage_list::rect edges[decor::OUTLINE_EDGES];
    decor::outline_edges(m_outline, edges);
    for (const damage_list::rect& edge : edges) {
        m_damage.add(edge.x, edge.y, edge.w, edge.h);
    }
}

/* A hover or press changes the look of its window's controls alone, so only they repaint */
void server::damage_controls(const dm_control& control) {
    if (!control.window) {
        return;
    }

    damage_list::rect r = decor::controls_rect(*control.window);
    m_damage.add(r.x, r.y, r.w, r.h);
}

void server::release_control_press() {
    damage_controls(m_control_press);
    m_control_press = {};
}

void server::act_on_control(dm_window* w, decor::zone control) {
    switch (control) {
    case decor::zone::minimize:
        minimize_window(w);
        break;
    case decor::zone::maximize:
        toggle_maximized(w);
        break;
    case decor::zone::close: {
        swp_event_rec rec;
        memset(&rec, 0, sizeof(rec));
        rec.kind = SWP_EV_CLOSE;
        send_event(w, rec);
        break;
    }
    case decor::zone::none:
    case decor::zone::content:
    case decor::zone::title:
    case decor::zone::resize_l:
    case decor::zone::resize_r:
    case decor::zone::resize_t:
    case decor::zone::resize_b:
    case decor::zone::resize_tl:
    case decor::zone::resize_tr:
    case decor::zone::resize_bl:
    case decor::zone::resize_br:
        break;
    }
}

/* Promised positions and a held resize's anchors move with the window, or
 * a late resize ack or the resize's next motion would snap it back */
void server::move_window(dm_window* w, int32_t dx, int32_t dy) {
    scene_damage_window(w);
    w->x += dx;
    w->y += dy;
    scene_damage_window(w);
    update_window_output(*w);

    for (uint32_t i = 0; i < w->sent_conf_count; i++) {
        w->sent_confs[i].x += dx;
        w->sent_confs[i].y += dy;
    }

    if (w->target_w != 0) {
        w->target_x += dx;
        w->target_y += dy;
    }

    if (w == m_resize) {
        damage_outline();
        m_resize_anchor_x += dx;
        m_resize_anchor_y += dy;
        m_outline.x += dx;
        m_outline.y += dy;
        damage_outline();
    }
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

    m_outline = decor::frame_rect(*w, w->x, w->y, static_cast<int32_t>(b.width), static_cast<int32_t>(b.height));
    damage_outline();
}

/* Recomputes the drag target from the pointer, keeping the opposite
 * edges anchored, and moves the rubber band to it */
void server::update_resize(int32_t px, int32_t py) {
    dm_window* w = m_resize;
    const dm_buffer& b = w->buffers[static_cast<size_t>(w->current)];

    uint32_t min_w = w->min_w > decor::MIN_CONTENT_W ? w->min_w : decor::MIN_CONTENT_W;
    uint32_t min_h = w->min_h > decor::MIN_CONTENT_H ? w->min_h : decor::MIN_CONTENT_H;

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
    bool is_last_release = changed != 0 && all_released;

    if (press) {
        m_last_press_ns = now_ns();
    }

    for (uint32_t btn = 0; btn < DM_BUTTONS; btn++) {
        uint16_t bit = static_cast<uint16_t>(1u << btn);
        if ((changed & bit) != 0 && (buttons & bit) == 0) {
            m_press_serials[btn] = 0;
        }
    }

    /* An active overlay owns the pointer entirely. Hover and hold
     * transitions repaint the orb boxes. */
    if (m_power->active()) {
        move_cursor(x, y, SWP_CURSOR_ARROW);

        int32_t old_hover = m_power->hover_choice();
        m_power->on_motion(x, y);
        if ((changed & 1) != 0) {
            if ((buttons & 1) != 0) {
                m_power->on_press(x, y);
            } else {
                m_power->on_release();
            }
        }

        if (m_power->hover_choice() != old_hover || (changed & 1) != 0) {
            for (int32_t i = 0; i < 2; i++) {
                damage_list::rect ob = m_power->orb_box(i);
                m_damage.add(ob.x, ob.y, ob.w, ob.h);
            }
        }
        return;
    }

    if (m_capture.active()) {
        capture_pointer(x, y, buttons, changed);
        return;
    }

    /* A preview takes the pointer whole, and the release of a click closes it, so nothing beneath hears either half */
    if (m_preview.active()) {
        move_cursor(x, y, SWP_CURSOR_ARROW);
        if (changed != 0 && !press) {
            close_preview();
        }

        return;
    }

    if (route_drag_pointer(x, y, buttons, changed, wheel)) {
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
            release_control_press();
        }
        return;
    }

    /* An active title drag owns the pointer until every button lifts, keeping
     * the title bar's rows inside the work area of the pointer's output. */
    if (m_drag) {
        move_cursor(x, y, m_cursor_shape);
        bool moves = restore_dragged_window(x, y);

        int32_t under_pointer = m_layout.output_at(x, y);
        uint32_t output = under_pointer >= 0 ? static_cast<uint32_t>(under_pointer) : m_layout.main_output();
        damage_list::rect work = m_layout.work_area(output);

        int32_t want_y = y - m_drag_dy;
        int32_t min_y = work.y + decor::TITLE_H;
        if (want_y < min_y) {
            want_y = min_y;
        }

        if (want_y > work.y + work.h) {
            want_y = work.y + work.h;
        }

        int32_t dx = x - m_drag_dx - m_drag->x;
        int32_t dy = want_y - m_drag->y;

        if (moves) {
            move_window(m_drag, dx, dy);
        }

        if (all_released) {
            m_drag = nullptr;
            release_control_press();
        }
        return;
    }

    /* A press the panels hold owns the pointer until its release, wherever the pointer goes meanwhile */
    if (m_panels->holds_pointer()) {
        move_cursor(x, y, m_cursor_shape);
        feed_panels(*m_panels, x, y, buttons, changed, wheel);
        return;
    }

    /* So does a press on a capture's thumbnail */
    if (m_thumbnails.holds_pointer()) {
        move_cursor(x, y, SWP_CURSOR_ARROW);
        thumbnail_pointer(x, y, buttons, changed);
        return;
    }

    /* A press that `dismisses_popover()` accepts closes the popover and reaches nothing else */
    if (press && m_panels->dismisses_popover(x, y)) {
        m_panels->close_popover();
        return;
    }

    /* A press outside a grabbing popup dismisses it and swallows the press */
    if (m_grab_popup && press && window_at(x, y, nullptr) != m_grab_popup) {
        dismiss_grab_popup();
        return;
    }

    decor::zone zone = decor::zone::none;
    dm_window* struck = m_grab ? m_grab : window_at(x, y, &zone);
    if (m_grab) {
        zone = decor::zone::content;
    }

    /* Over the shell's popover or a capture's thumbnail the pointer reaches no window, unless one holds the grab */
    bool is_over_popover = !m_grab && m_panels->popover_contains(x, y);
    bool is_over_thumbnail = !m_grab && !is_over_popover && m_thumbnails.contains(x, y);
    if (is_over_popover || is_over_thumbnail) {
        struck = nullptr;
        zone = decor::zone::none;
    }

    if (!is_over_thumbnail) {
        m_thumbnails.pointer_move(-1, -1, now_ns(), m_damage);
    }

    /* The shape follows the content window's request, and the frame
     * band advertises the resize direction it would grab */
    uint8_t resize_edges = zone_edges(zone);
    uint32_t shape = zone == decor::zone::content && struck
                   ? struck->cursor
                   : resize_edges != 0 ? zone_shape(zone)
                                       : SWP_CURSOR_ARROW;
    move_cursor(x, y, shape);

    dm_control hovered;
    if (decor::is_control(zone)) {
        hovered = { struck, zone };
    }

    if (hovered != m_control_hover) {
        damage_controls(m_control_hover);
        m_control_hover = hovered;
        damage_controls(m_control_hover);
    }

    /* Decoration interactions never reach the client */
    bool is_title = zone == decor::zone::title;
    bool is_resize_band = resize_edges != 0;
    if (is_title || decor::is_control(zone) || is_resize_band) {
        bool is_on_pressed_control = decor::is_control(zone) && m_control_press == hovered;
        if (press) {
            set_focus(struck);
            scene_raise(struck);

            if (is_resize_band) {
                begin_resize(struck, zone, shape);
            } else if (is_title) {
                bool is_double_click = record_title_press(struck, x, y);
                if (is_double_click) {
                    toggle_maximized(struck);
                } else {
                    m_drag = struck;
                    m_drag_dx = x - struck->x;
                    m_drag_dy = y - struck->y;
                }
            } else {
                m_control_press = hovered;
                damage_controls(m_control_press);
            }
        } else if (changed != 0 && is_on_pressed_control) {
            release_control_press();

            /* A control acts on its release, so a person can cancel a press by releasing elsewhere */
            act_on_control(struck, zone);
        }

        /* A press that ended anywhere else sheds the pressed shade */
        if (is_last_release && m_control_press.window) {
            release_control_press();
        }
        return;
    }

    /* The release that ends a press on a window's control never reaches the popover */
    if (is_last_release && m_control_press.window) {
        release_control_press();
        if (is_over_popover) {
            return;
        }
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
            rec.modifiers = m_modifiers;
            rec.x = x - target->x;
            rec.y = y - target->y;
            send_event(target, rec);
        }
    }

    if (!target) {
        if (is_over_thumbnail) {
            m_panels->pointer_move(-1, -1);
            thumbnail_pointer(x, y, buttons, changed);
            return;
        }

        /* Uncovered band area belongs to the panels, and a motion
         * elsewhere clears any panel hover */
        if (!struck && m_panels->contains(x, y)) {
            feed_panels(*m_panels, x, y, buttons, changed, wheel);
        } else {
            m_panels->pointer_move(-1, -1);
        }

        return;
    }

    m_panels->pointer_move(-1, -1);

    swp_event_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.modifiers = m_modifiers;
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

    for (uint8_t btn = 0; btn < DM_BUTTONS; btn++) {
        uint16_t bit = static_cast<uint16_t>(1u << btn);
        if (!(changed & bit)) {
            continue;
        }

        bool down = (buttons & bit) != 0;
        rec.kind = down ? SWP_EV_BUTTON_DOWN : SWP_EV_BUTTON_UP;
        rec.button = btn;
        rec.scroll = 0;
        rec.ch = 0;

        if (down) {
            set_focus(target);
            scene_raise(target);
            m_grab = target;

            m_press_serial = m_press_serial == UINT32_MAX ? 1 : m_press_serial + 1;
            m_press_serials[btn] = m_press_serial;
            m_press_x[btn] = x;
            m_press_y[btn] = y;
            rec.ch = m_press_serial;
        } else if (m_grab && all_released) {
            m_grab = nullptr;
        }

        send_event(target, rec);
    }
}
