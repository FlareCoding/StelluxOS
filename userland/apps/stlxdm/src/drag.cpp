#include "server.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* DROP_GETs one client may leave unanswered, which only a client that asks without reading its socket reaches */
constexpr size_t DROP_ASKS_MAX = 64;

/* The modifier keys, which reach the focused window during a drag, and Esc, which cancels it */
constexpr uint16_t HID_FIRST_MODIFIER = 0xE0;
constexpr uint16_t HID_LAST_MODIFIER = 0xE7;
constexpr uint16_t HID_USAGE_ESCAPE = 0x29;

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

/* The picture a pose shows, where a settle or a snap-back starts */
static dnd_frame frame_of(const drag_pose& pose) {
    return { pose.picture, pose.opacity, pose.shadow_opacity };
}

static bool overlaps(const damage_list::rect& a, const damage_list::rect& b) {
    damage_list::rect common = damage_list::intersect(a, b);
    return common.w > 0 && common.h > 0;
}

/* The oldest queued handle, -1 when none is queued */
static int take_queued_handle(dm_client& c) {
    if (c.queued_handle_count == 0) {
        return -1;
    }

    int handle = c.queued_handles[0];
    c.queued_handle_count--;
    memmove(c.queued_handles, c.queued_handles + 1, c.queued_handle_count * sizeof(int));

    return handle;
}

/* Whether each representation's object holds the bytes its length promises */
static bool holds_lengths(const clipboard_item& item) {
    for (const clipboard_rep& rep : item.reps()) {
        struct stat st;
        bool is_short = rep.object >= 0 && (fstat(rep.object, &st) != 0 || static_cast<uint64_t>(st.st_size) < rep.len);
        if (is_short) {
            return false;
        }
    }

    return true;
}

/* Copies a START_DRAG's picture out of its object, which must hold every row, its hotspot kept inside it */
static bool copy_drag_picture(int object, const swp_start_drag& m, dnd_session& session) {
    size_t count = static_cast<size_t>(m.picture_w) * m.picture_h;
    size_t bytes = count * sizeof(uint32_t);
    struct stat st;
    if (fstat(object, &st) != 0 || static_cast<uint64_t>(st.st_size) < bytes) {
        return false;
    }

    void* shared = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, object, 0);
    if (shared == MAP_FAILED) {
        return false;
    }

    const uint32_t* pixels = static_cast<const uint32_t*>(shared);
    session.picture.assign(pixels, pixels + count);
    munmap(shared, bytes);

    session.picture_w = m.picture_w;
    session.picture_h = m.picture_h;
    session.hot_x = std::clamp(m.hot_x, 0, static_cast<int32_t>(m.picture_w) - 1);
    session.hot_y = std::clamp(m.hot_y, 0, static_cast<int32_t>(m.picture_h) - 1);

    return true;
}

/* A malformed START_DRAG ends its client, and one that comes after its press, for another press, during another drag
 * or while something else holds the input is refused */
void server::handle_start_drag(dm_client& c, const uint8_t* payload) {
    swp_start_drag m;
    memcpy(&m, payload, sizeof(m));

    std::vector<clipboard_rep> reps;
    if (!read_start_drag(m, &reps)) {
        c.dead = true;
        return;
    }

    int picture = -1;
    if (m.picture_w != 0) {
        picture = take_queued_handle(c);
        if (picture < 0) {
            c.dead = true;
            return;
        }
    }

    if (!take_queued_handles(c, reps)) {
        if (picture >= 0) {
            close(picture);
        }

        return;
    }

    auto session = std::make_unique<dnd_session>();
    session->item = clipboard_item(std::move(reps), 0);
    bool has_picture = picture < 0 || copy_drag_picture(picture, m, *session);
    if (picture >= 0) {
        close(picture);
    }

    if (!has_picture || !holds_lengths(session->item)) {
        c.dead = true;
        return;
    }

    dm_window* w = find_window(c, m.win_id);
    uint32_t button = 0;
    if (!may_start_drag(w, m.serial, &button)) {
        swp_drag_state refused = { m.win_id, m.serial, SWP_DRAG_REFUSED, 0 };
        send_window_message(c, w, SWP_MSG_DRAG_STATE, &refused, sizeof(refused));
        return;
    }

    session->drag = m.serial;
    session->button = button;
    session->actions = m.actions;
    session->item_count = m.item_count;

    session->press_x = m_press_x[button];
    session->press_y = m_press_y[button];
    start_drag(c, w, std::move(session));
}

/* A drag starts only from a press its window still holds as the pointer grab, while no drag runs and nothing else
 * holds the input */
bool server::may_start_drag(const dm_window* w, uint32_t serial, uint32_t* out_button) {
    bool is_input_free = !m_dnd && !m_power->active() && !m_panels->holds_pointer() && !m_drag && !m_resize;
    if (!w || w != m_grab || serial == 0 || !is_input_free) {
        return false;
    }

    for (uint32_t btn = 0; btn < DM_BUTTONS; btn++) {
        if (m_press_serials[btn] == serial) {
            *out_button = btn;
            return true;
        }
    }

    return false;
}

/* The drag takes the pointer from the window holding it, which hears it leave, and from a grabbing popup the drag
 * did not start from */
void server::start_drag(dm_client& c, dm_window* w, std::unique_ptr<dnd_session> session) {
    session->source = &c;
    session->source_win_id = w->win_id;

    session->x = m_cursor_x;
    session->y = m_cursor_y;
    session->buttons = static_cast<uint16_t>(1u << session->button);
    session->modifiers = m_modifiers;
    m_dnd = std::move(session);

    m_grab = nullptr;
    if (m_grab_popup && m_grab_popup != w) {
        dismiss_grab_popup();
    }

    if (m_hover) {
        swp_event_rec rec;
        memset(&rec, 0, sizeof(rec));
        rec.kind = SWP_EV_LEAVE;
        send_event(m_hover, rec);
        m_hover = nullptr;
    }

    damage_controls(m_control_hover);
    m_control_hover = {};
    m_panels->pointer_move(-1, -1);

    swp_drag_state started = { w->win_id, m_dnd->drag, SWP_DRAG_STARTED, 0 };
    send_window_message(c, w, SWP_MSG_DRAG_STATE, &started, sizeof(started));

    /* The look of a drag before, still settling or snapping back, gives way to this one */
    damage_drag();
    if (!m_drag_look.draw_picture(frame_maker(), *m_dnd)) {
        printf("stlxdm: drag %u shows no picture, as memory ran out\r\n", m_dnd->drag);
    }

    m_drag_shown = true;
    m_drag_motion = lift_motion(m_drag_look.picture_at(m_dnd->press_x, m_dnd->press_y), now_ns());
    repose_drag();

    move_cursor(m_cursor_x, m_cursor_y, SWP_CURSOR_ARROW);
    refresh_cursor_planes();
    update_drag_target();
}

/* A cancel from the source ends a drag still following the pointer, and is ignored once it dropped */
void server::handle_cancel_drag(dm_client& c, const uint8_t* payload) {
    swp_cancel_drag m;
    memcpy(&m, payload, sizeof(m));

    bool is_own_drag = m_dnd && m_dnd->source == &c && m_dnd->drag == m.drag;
    if (!is_own_drag || m_dnd->phase != dnd_session::stage::dragging) {
        return;
    }

    if (m_dnd->target) {
        send_drop_leave(m_dnd->target);
    }

    uint16_t buttons = m_dnd->buttons;
    end_drag(SWP_DRAG_CANCELLED, 0);
    resume_pointer(buttons);
}

/* Only the window under the pointer answers, and only for the drag over it */
void server::handle_drop_status(dm_client& c, const uint8_t* payload) {
    swp_drop_status m;
    memcpy(&m, payload, sizeof(m));

    dm_window* w = find_window(c, m.win_id);
    bool is_target = m_dnd && m_dnd->phase == dnd_session::stage::dragging && w && w == m_dnd->target;
    if (!is_target || m.drag != m_dnd->drag) {
        return;
    }

    m_dnd->answer = read_drop_status(m);
    refresh_drag_action();
}

void server::handle_drop_get(dm_client& c, const uint8_t* payload) {
    if (c.drop_asks.size() == DROP_ASKS_MAX) {
        c.dead = true;
        return;
    }

    swp_drop_get m;
    memcpy(&m, payload, sizeof(m));

    c.drop_asks.push_back({ m.win_id, m.drag, std::string(m.mime, strnlen(m.mime, sizeof(m.mime))) });
    send_drop_answers(c);
}

/* The window that took the drop ends it, and an action the drag did not choose counts as none */
void server::handle_drop_finish(dm_client& c, const uint8_t* payload) {
    swp_drop_finish m;
    memcpy(&m, payload, sizeof(m));

    dm_window* w = find_window(c, m.win_id);
    bool is_target = m_dnd && m_dnd->phase == dnd_session::stage::dropped && w && w == m_dnd->target;
    if (!is_target || m.drag != m_dnd->drag) {
        return;
    }

    uint32_t action = m.action == m_dnd->action ? m.action : 0;
    if ((m.flags & SWP_DROP_FOCUS) != 0 && action != 0) {
        set_focus(w);
        scene_raise(w);
    }

    end_drag(SWP_DRAG_FINISHED, action);
}

/* While a drag follows the pointer, motion moves it over the windows beneath, the wheel still scrolls the window under
 * it, and the release of the press's button drops it. True when the drag took the input. */
bool server::route_drag_pointer(int32_t x, int32_t y, uint16_t buttons, uint16_t changed, int16_t wheel) {
    if (!m_dnd || m_dnd->phase != dnd_session::stage::dragging) {
        return false;
    }

    dnd_session& s = *m_dnd;
    damage_drag();
    s.x = x;
    s.y = y;
    s.buttons = buttons;
    s.motion_pending = true;

    move_cursor(x, y, SWP_CURSOR_ARROW);
    update_drag_target();
    repose_drag();
    refresh_drag_carry();

    if (wheel != 0) {
        scroll_under_drag(x, y, wheel);
    }

    uint16_t bit = static_cast<uint16_t>(1u << s.button);
    if ((changed & bit) != 0 && (buttons & bit) == 0) {
        drop_drag();
        resume_pointer(buttons);
    }

    return true;
}

/* During a drag Esc cancels, the modifier keys change the action and still reach the focused window, a shortcut that
 * starts a program still starts it, and every other key is swallowed. True when the drag took the key. */
bool server::route_drag_key(uint16_t usage, bool down, bool repeat) {
    if (!m_dnd || m_dnd->phase != dnd_session::stage::dragging) {
        return false;
    }

    dnd_session& s = *m_dnd;
    if (s.modifiers != m_modifiers) {
        s.modifiers = m_modifiers;
        s.motion_pending = true;
        refresh_drag_action();
    }

    if (usage >= HID_FIRST_MODIFIER && usage <= HID_LAST_MODIFIER) {
        return false;
    }

    bool is_press = down && !repeat;
    if (is_press && usage == HID_USAGE_ESCAPE) {
        if (s.target) {
            send_drop_leave(s.target);
        }

        uint16_t buttons = s.buttons;
        end_drag(SWP_DRAG_CANCELLED, 0);
        resume_pointer(buttons);
        return true;
    }

    for (const dm_hotkey& hk : m_hotkeys) {
        if (is_press && hk.kind == STLXCONF_ACTION_EXEC && hk.usage == usage && hk.mods == m_modifiers) {
            run_shortcut(hk);
            break;
        }
    }

    return true;
}

/* The window whose content lies under the point, where a drag may drop: none over a frame, the shell's popover or an
 * animating window */
dm_window* server::drag_target_at(int32_t x, int32_t y) {
    if (m_panels->popover_contains(x, y)) {
        return nullptr;
    }

    decor::zone zone = decor::zone::none;
    dm_window* w = window_at(x, y, &zone);

    return zone == decor::zone::content ? w : nullptr;
}

/* Finds the window under the pointer again, telling the one it leaves and the one it enters, which refuses until it
 * answers */
void server::update_drag_target() {
    dnd_session& s = *m_dnd;
    dm_window* under = drag_target_at(s.x, s.y);
    if (under == s.target) {
        return;
    }

    if (s.target) {
        send_drop_leave(s.target);
    }

    s.target = under;
    s.answer = {};
    refresh_drag_action();
    if (under) {
        send_drop_enter(under);
        s.motion_pending = false;
    }
}

/* The action a release does now, and the chip and opacity that show it */
void server::refresh_drag_action() {
    dnd_session& s = *m_dnd;
    uint32_t action = s.target ? choose_dnd_action(s.actions, s.answer, s.modifiers) : 0;
    dnd_chip chip = chip_for(action, s.answer);
    if (action == s.action && chip == m_drag_look.chip()) {
        return;
    }

    damage_drag();
    s.action = action;
    if (!(chip == m_drag_look.chip()) && !m_drag_look.draw_chip(chip)) {
        printf("stlxdm: drag %u shows no chip, as memory ran out\r\n", s.drag);
    }

    repose_drag();
    refresh_drag_carry();
}

void server::scroll_under_drag(int32_t x, int32_t y, int16_t wheel) {
    dm_window* under = drag_target_at(x, y);
    if (!under) {
        return;
    }

    swp_event_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.kind = SWP_EV_SCROLL;
    rec.modifiers = m_modifiers;
    rec.scroll = wheel;

    rec.x = x - under->x;
    rec.y = y - under->y;
    send_event(under, rec);
}

void server::send_drop_enter(dm_window* w) {
    dm_client* owner = window_owner(w);
    if (!owner) {
        return;
    }

    const dnd_session& s = *m_dnd;
    swp_clipboard_types types = describe_types(s.item);

    swp_drop_enter m;
    memset(&m, 0, sizeof(m));
    m.win_id = w->win_id;
    m.drag = s.drag;
    m.x = s.x - w->x;
    m.y = s.y - w->y;

    m.actions = s.actions;
    m.item_count = s.item_count;
    m.flags = owner == s.source ? SWP_DROP_OWN : 0;
    m.modifiers = s.modifiers;

    m.count = types.count;
    memcpy(m.types, types.types, sizeof(m.types));
    send_window_message(*owner, w, SWP_MSG_DROP_ENTER, &m, sizeof(m));
}

void server::send_drop_leave(dm_window* w) {
    dm_client* owner = window_owner(w);
    if (!owner) {
        return;
    }

    swp_drop_leave m = { w->win_id, m_dnd->drag };
    send_window_message(*owner, w, SWP_MSG_DROP_LEAVE, &m, sizeof(m));
}

/* A release over a window whose newest answer accepts drops there, and anywhere else cancels */
void server::drop_drag() {
    dnd_session& s = *m_dnd;
    dm_window* w = s.target;
    dm_client* owner = w ? window_owner(w) : nullptr;
    if (!owner || s.action == 0) {
        if (owner) {
            send_drop_leave(w);
        }

        end_drag(SWP_DRAG_CANCELLED, 0);
        return;
    }

    swp_drop m = { w->win_id, s.drag, s.x - w->x, s.y - w->y, s.action, s.modifiers };
    send_window_message(*owner, w, SWP_MSG_DROP, &m, sizeof(m));

    uint64_t now = now_ns();
    s.phase = dnd_session::stage::dropped;
    s.motion_pending = false;
    s.finish_deadline_ns = now + DND_FINISH_TIMEOUT_NS;

    damage_drag();
    m_drag_motion = settle_motion(frame_of(m_drag_pose), s.x, s.y, now);
    repose_drag();
    refresh_cursor_planes();
}

/* Tells the source how its drag ended, sends back a picture still at the pointer, and closes the objects held for
 * the drag */
void server::end_drag(uint32_t state, uint32_t action) {
    dnd_session& s = *m_dnd;
    if (drag_follows_pointer()) {
        return_drag();
    }

    if (s.source) {
        swp_drag_state m = { s.source_win_id, s.drag, state, action };
        send_window_message(*s.source, find_window(*s.source, s.source_win_id), SWP_MSG_DRAG_STATE, &m, sizeof(m));
    }

    m_dnd.reset();
    refresh_cursor_planes();
}

/* A drag that drops nowhere slides back to its item, or fades where it is once its source window is gone or
 * minimized */
void server::return_drag() {
    const dnd_session& s = *m_dnd;
    dm_window* source = s.source ? find_window(*s.source, s.source_win_id) : nullptr;
    bool is_source_shown = source && source->placement.minimized_ns == 0;
    damage_list::rect item = is_source_shown ? m_drag_look.picture_at(s.press_x, s.press_y) : damage_list::rect{};

    damage_drag();
    m_drag_motion = snap_back_motion(frame_of(m_drag_pose), item, now_ns());
    repose_drag();
}

/* The pointer goes back to the windows the moment a drag stops following it, so the one under it hears it enter */
void server::resume_pointer(uint16_t buttons) {
    route_pointer(m_cursor_x, m_cursor_y, buttons, 0, 0);
}

/* Finds the target again for a scene that changed under a still pointer, and sends the wakeup's one DROP_MOTION */
void server::flush_drag() {
    if (!m_dnd || m_dnd->phase != dnd_session::stage::dragging) {
        return;
    }

    update_drag_target();
    dnd_session& s = *m_dnd;
    dm_client* owner = s.target ? window_owner(s.target) : nullptr;
    if (!s.motion_pending || !owner) {
        s.motion_pending = false;
        return;
    }

    s.motion_pending = false;
    swp_drop_motion m = { s.target->win_id, s.drag, s.x - s.target->x, s.y - s.target->y, s.modifiers };
    send_window_message(*owner, s.target, SWP_MSG_DROP_MOTION, &m, sizeof(m));
}

/* A target that goes away stops being one, and one that took the drop counts as having done nothing */
void server::forget_drag_window(const dm_window* w) {
    if (!m_dnd || m_dnd->target != w) {
        return;
    }

    if (m_dnd->phase == dnd_session::stage::dropped) {
        end_drag(SWP_DRAG_FINISHED, 0);
        return;
    }

    m_dnd->target = nullptr;
    m_dnd->answer = {};
    refresh_drag_action();
}

/* A drag whose source goes away runs on with the objects stlxdm holds, and nobody hears how it ends */
void server::forget_drag_client(const dm_client& c) {
    if (m_dnd && m_dnd->source == &c) {
        m_dnd->source = nullptr;
    }
}

int64_t server::drag_timeout_ns(uint64_t now) const {
    if (!m_dnd || m_dnd->phase != dnd_session::stage::dropped) {
        return -1;
    }

    return m_dnd->finish_deadline_ns > now ? static_cast<int64_t>(m_dnd->finish_deadline_ns - now) : 0;
}

void server::drag_tick(uint64_t now) {
    bool is_overdue = m_dnd && m_dnd->phase == dnd_session::stage::dropped && now >= m_dnd->finish_deadline_ns;
    if (!is_overdue) {
        return;
    }

    printf("stlxdm: window %u took a drop and never finished it, so it did nothing\r\n", m_dnd->target->win_id);
    end_drag(SWP_DRAG_FINISHED, 0);
}

/* The representation an ask names, only for the window that took the drop, from its DROP until it finishes */
const clipboard_rep* server::drop_rep_for(dm_client& c, const dm_drop_ask& ask) {
    if (!m_dnd || m_dnd->phase != dnd_session::stage::dropped || m_dnd->drag != ask.drag) {
        return nullptr;
    }

    dm_window* w = m_dnd->target;
    if (!w || w->win_id != ask.win_id || window_owner(w) != &c) {
        return nullptr;
    }

    const clipboard_rep* rep = m_dnd->item.find(ask.mime_type);

    return rep && rep->object >= 0 ? rep : nullptr;
}

bool server::send_drop_answer(dm_client& c) {
    const clipboard_rep* rep = drop_rep_for(c, c.drop_asks.front());
    int object = rep ? read_only_copy(rep->object) : -1;
    swp_drop_data m = { object >= 0 ? rep->len : 0 };

    bool is_sent = send_with_handle(c, SWP_MSG_DROP_DATA, &m, sizeof(m), object);
    if (object >= 0) {
        close(object);
    }

    return is_sent;
}

void server::send_drop_answers(dm_client& c) {
    while (!c.drop_asks.empty() && c.out_q.empty() && !c.dead) {
        if (!send_drop_answer(c)) {
            return;
        }

        c.drop_asks.pop_front();
    }
}

/* Whether a drag's look is on screen, from its start until its picture settles or snaps back */
bool server::drag_shows() const {
    return m_drag_shown;
}

/* Whether a drag's picture follows the pointer, until the release or the cancel */
bool server::drag_follows_pointer() const {
    return m_dnd && m_dnd->phase == dnd_session::stage::dragging;
}

/* Whether a drag's picture moves apart from the pointer, which the paced tick steps */
bool server::drag_moves() const {
    return m_drag_shown && m_drag_motion.type != dnd_motion::kind::none;
}

/* Whole while a release would drop, and faded while nothing under the pointer accepts */
uint32_t server::drag_opacity() const {
    return m_dnd->action != 0 ? decor::CHANNEL_MAX : DND_REFUSED_OPACITY;
}

/* The place of the output under the pointer, whose edges the chip keeps inside */
damage_list::rect server::drag_output() const {
    int32_t under = m_layout.output_at(m_dnd->x, m_dnd->y);
    uint32_t output = under >= 0 ? static_cast<uint32_t>(under) : m_layout.main_output();

    return m_layout.find(output)->rect;
}

/* The look at `now`: riding the pointer with the chip beside it, lifting toward it, or settling or snapping back
 * apart from it */
drag_pose server::pose_drag(uint64_t now) const {
    bool rides = m_drag_motion.type == dnd_motion::kind::none || m_drag_motion.type == dnd_motion::kind::lift;
    bool follows = rides && drag_follows_pointer();
    damage_list::rect at_pointer = follows ? m_drag_look.picture_at(m_dnd->x, m_dnd->y) : damage_list::rect{};
    dnd_frame frame = step_dnd_motion(m_drag_motion, now, at_pointer, follows ? drag_opacity() : 0);

    drag_pose pose;
    pose.picture = frame.picture;
    pose.opacity = frame.opacity;
    pose.shadow_opacity = frame.shadow_opacity;
    if (follows) {
        pose.has_chip = true;
        pose.pointer_x = m_dnd->x;
        pose.pointer_y = m_dnd->y;
        pose.output = drag_output();
    }

    return pose;
}

/* Marks where the drag's look lies for the next frames to repaint, unless a hardware cursor carries it */
void server::damage_drag() {
    if (m_drag_carrier < 0) {
        damage_drag_place();
    }
}

void server::damage_drag_place() {
    if (!m_drag_shown) {
        return;
    }

    damage_list::rect r = m_drag_look.bounds(m_drag_pose);
    if (r.w > 0 && r.h > 0) {
        m_damage.add(r.x, r.y, r.w, r.h);
    }
}

/* Shows the look where it belongs now, once damage_drag() marked where it was */
void server::repose_drag() {
    if (!m_drag_shown) {
        return;
    }

    m_drag_pose = pose_drag(now_ns());
    damage_drag();
}

/* Steps the look's motion on the paced tick: a lift hands the picture to the pointer, and the look leaves once a
 * settle, a fade or a snap-back ends */
void server::advance_drag_motion() {
    if (!drag_moves()) {
        return;
    }

    bool has_ended = has_dnd_motion_ended(m_drag_motion, now_ns());
    if (has_ended && m_drag_motion.type != dnd_motion::kind::lift) {
        hide_drag();
        return;
    }

    damage_drag();
    if (has_ended) {
        m_drag_motion = {};
    }

    repose_drag();
    refresh_drag_carry();
}

void server::hide_drag() {
    damage_drag();
    m_drag_look = drag_look();
    m_drag_motion = {};
    m_drag_shown = false;
}

/* A switch of compositors drops the look, which is drawn again for the compositor composing now, unless its session
 * already ended and took the source's pixels with it */
void server::remake_drag_look() {
    if (!m_drag_shown || m_drag_look.maker() == &frame_maker()) {
        return;
    }

    if (!m_dnd) {
        hide_drag();
        return;
    }

    m_drag_look.draw_picture(frame_maker(), *m_dnd);
    m_drag_look.draw_chip(chip_for(m_dnd->action, m_dnd->answer));
    repose_drag();
    refresh_drag_carry();
}

/* While a drag follows the pointer each output draws the pointer into its frames, so it moves with the picture, unless
 * its hardware cursor carries both, and once the drag stops each output takes back its hardware cursor where it can */
void server::refresh_cursor_planes() {
    for (dm_output& out : m_outputs) {
        if (out.pres && out.cursor_overlaps) {
            out.cursor_in_frames = !show_hardware_cursor(out);
            out.cursor_move_pending = false;
        }
    }

    set_drag_carrier(drag_carrier_shown());
    damage_cursor();
}

/* What the drag's cursor picture shows in the pose the frames would show */
dm_drag_cursor_key server::drag_cursor_key() const {
    const drag_pose& pose = m_drag_pose;
    dm_drag_cursor_key key;
    key.generation = m_drag_look.generation();
    if (pose.picture.w > 0 && pose.picture.h > 0) {
        key.picture_dx = pose.picture.x - pose.pointer_x;
        key.picture_dy = pose.picture.y - pose.pointer_y;
    }

    damage_list::rect chip = m_drag_look.chip_at(pose);
    if (chip.w > 0 && chip.h > 0) {
        key.chip_dx = chip.x - pose.pointer_x;
        key.chip_dy = chip.y - pose.pointer_y;
    }

    key.opacity = pose.opacity;
    key.shadow_opacity = pose.shadow_opacity;
    return key;
}

/* Whether `out`'s hardware cursor may carry the drag's look and the pointer, which cover `area`: the picture rides the
 * pointer after its lift, no overlay dims the frames, the look lies on no other output, and the cursor holds it */
bool server::may_carry_drag(const dm_output& out, const damage_list::rect& area) const {
    bool rides = drag_follows_pointer() && m_drag_motion.type == dnd_motion::kind::none && !m_power->active();
    bool holds = out.pres && overlaps(area, out.place) &&
                 out.pres->holds_cursor(static_cast<uint32_t>(area.w), static_cast<uint32_t>(area.h));
    if (!rides || !holds) {
        return false;
    }

    for (const dm_output& other : m_outputs) {
        if (&other != &out && other.pres && overlaps(area, other.place)) {
            return false;
        }
    }

    return true;
}

/* The output whose hardware cursor should carry the drag's look and the pointer now, -1 for none */
int32_t server::drag_carrier_wanted() const {
    presenter::cursor_picture pointer;
    if (!m_drag_shown || !m_cursor.picture(m_cursor_shape, pointer)) {
        return -1;
    }

    damage_list::rect area = m_drag_look.cursor_bounds(m_drag_pose, pointer);
    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        if (may_carry_drag(m_outputs[output], area)) {
            return static_cast<int32_t>(output);
        }
    }

    return -1;
}

/* Shows the drag's look with the pointer on `out`'s hardware cursor where it may carry them, building the picture
 * again once what it shows changed, or hides the cursor and returns false, so the frames draw both */
bool server::show_drag_cursor(dm_output& out) {
    presenter::cursor_picture pointer;
    bool has_pointer = m_drag_shown && m_cursor.picture(m_cursor_shape, pointer);
    damage_list::rect area = has_pointer ? m_drag_look.cursor_bounds(m_drag_pose, pointer) : damage_list::rect{};
    bool shown = has_pointer && may_carry_drag(out, area);

    dm_drag_cursor_key key = drag_cursor_key();
    bool is_stale = m_drag_cursor.pixels.empty() || !(key == m_drag_cursor.key);
    if (shown && is_stale) {
        for (dm_output& showing : m_outputs) {
            if (!m_drag_cursor.pixels.empty() && showing.cursor_picture == m_drag_cursor.pixels.data()) {
                showing.cursor_picture = nullptr;
            }
        }

        m_drag_look.cursor_picture(m_drag_pose, pointer, m_drag_cursor.pixels, m_drag_cursor.picture);
        m_drag_cursor.key = key;
    }

    shown = shown && out.pres->move_cursor(m_cursor_x - out.place.x, m_cursor_y - out.place.y);
    if (shown && out.cursor_picture != m_drag_cursor.pixels.data()) {
        shown = out.pres->set_cursor(m_drag_cursor.picture);
    }

    if (!shown) {
        out.pres->hide_cursor();
    }

    out.cursor_picture = shown ? m_drag_cursor.pixels.data() : nullptr;
    return shown;
}

/* The output whose hardware cursor shows the drag's picture while the drag rides the pointer, -1 for none */
int32_t server::drag_carrier_shown() const {
    if (!drag_follows_pointer() || m_drag_cursor.pixels.empty()) {
        return -1;
    }

    for (uint32_t output = 0; output < gfxfb::GFXFB_OUTPUTS_MAX; output++) {
        const dm_output& out = m_outputs[output];
        if (out.pres && out.cursor_picture == m_drag_cursor.pixels.data()) {
            return static_cast<int32_t>(output);
        }
    }

    return -1;
}

/* Notes which output carries the drag's look, repainting its place as it moves between the frames and a cursor, and
 * lets the cursor picture go once the drag no longer rides the pointer */
void server::set_drag_carrier(int32_t carrier) {
    bool was_carried = m_drag_carrier >= 0;
    m_drag_carrier = carrier;
    if (was_carried != (carrier >= 0)) {
        damage_drag_place();
    }

    if (!drag_follows_pointer()) {
        m_drag_cursor = {};
    }
}

/* Moves the drag's look between the frames and a hardware cursor once the cursor that should carry it, or what that
 * cursor would show, changed, so a motion that changes neither composes nothing and calls no cursor */
void server::refresh_drag_carry() {
    int32_t wanted = drag_carrier_wanted();
    dm_drag_cursor_key key = drag_cursor_key();
    bool is_unchanged = wanted == m_drag_cursor.tried_carrier && key == m_drag_cursor.tried_key;
    if (is_unchanged) {
        return;
    }

    m_drag_cursor.tried_carrier = wanted;
    m_drag_cursor.tried_key = key;
    if (wanted >= 0 || m_drag_carrier >= 0) {
        refresh_cursor_planes();
    }
}
