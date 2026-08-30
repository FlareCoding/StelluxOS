#ifndef STLXDM_SERVER_HPP
#define STLXDM_SERVER_HPP

#include "cursor.hpp"
#include "damage.hpp"
#include "decor.hpp"
#include "panels.hpp"
#include "power.hpp"
#include "presenter.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/surface.h>
#include <stlxwin/proto.h>

#include <cstdint>
#include <memory>
#include <vector>

/* Staged event records per window, flushed as one EVENT message per
 * wakeup. A full batch flushes early. */
constexpr uint32_t DM_EV_BATCH_MAX = 16;

/* One parsed exec shortcut: required modifier bits and the usage
 * that fires it while they are held */
struct dm_hotkey {
    uint8_t mods = 0;
    uint16_t usage = 0;
    const char* path = nullptr;
};

/* Config-derived state built as a unit and adopted only when every
 * piece came up, so a failed reload keeps the running desktop */
struct dm_conf_state {
    std::unique_ptr<dm_panels> panels;
    std::unique_ptr<dm_power> power;
    stlxgfx_surface_t* wallpaper = nullptr;
    std::vector<dm_hotkey> hotkeys;
};

/* One attached shared-memory buffer, mapped for the window's lifetime
 * or until the client detaches it. */
struct dm_buffer {
    uint32_t  buf_id = 0;
    uint32_t  width = 0, height = 0;
    size_t    size = 0;
    uint32_t* pixels = nullptr;
};

/* One sent and not yet acked CONFIGURE, remembered so the acking
 * commit can be validated and the window repositioned to the spot
 * this size was promised for. */
struct dm_sent_conf {
    uint32_t serial = 0;
    uint32_t w = 0, h = 0;
    int32_t  x = 0, y = 0;
};

/* One window. Displayed geometry is always the current buffer's
 * geometry, pending holds the newest unlatched commit. */
struct dm_window {
    uint32_t win_id = 0;
    uint32_t parent_id = 0;
    int32_t  x = 0, y = 0;
    uint32_t flags = 0;
    uint32_t popup_flags = 0;
    char     title[SWP_TITLE_MAX] = {};

    std::vector<dm_buffer> buffers;
    int32_t  pending = -1;      /* buffers index awaiting latch */
    int32_t  current = -1;      /* buffers index on screen */
    uint32_t sent_serial = 0;
    uint32_t acked_serial = 0;
    uint32_t cursor = 0;        /* swp_cursor, drawn once the sprite lands */
    bool     mapped = false;    /* first commit latched */
    uint32_t min_w = 0, min_h = 0;  /* client size hints, 0 unset */
    uint32_t max_w = 0, max_h = 0;

    /* Desired geometry not yet sent as a CONFIGURE, target_w of zero
     * means settled. The ledger holds sent unacked configures. */
    uint32_t target_w = 0, target_h = 0;
    int32_t  target_x = 0, target_y = 0;
    static constexpr uint32_t SENT_CONF_MAX = 8;
    dm_sent_conf sent_confs[SENT_CONF_MAX];
    uint32_t sent_conf_count = 0;

    swp_event_rec ev_batch[DM_EV_BATCH_MAX];
    uint32_t ev_batch_count = 0;

    /* Damage carried by the pending commit, buffer coordinates.
     * Zero rects means the whole buffer changed. */
    swp_rect pending_damage[SWP_COMMIT_MAX_RECTS];
    uint32_t pending_damage_count = 0;
};

/* One connected application. Owns the socket, the partial-message
 * assembly buffer, and its windows. */
struct dm_client {
    int      fd = -1;
    bool     hello_done = false;
    bool     dead = false;
    char     app_id[SWP_APP_ID_MAX] = {};

    uint8_t  rd_buf[SWP_MAX_MSG_SIZE];
    uint32_t rd_have = 0;

    /* Outbound bytes the socket would not take, bounded, flushed on
     * POLLOUT. The tail marker enables in-place motion coalescing. */
    std::vector<uint8_t> out_q;
    size_t   q_motion_off = SIZE_MAX;
    uint32_t q_motion_win = 0;

    std::vector<std::unique_ptr<dm_window>> windows;

    explicit dm_client(int sock) : fd(sock) {}
};

/* The protocol server: listens, accepts, pumps client sockets, routes
 * complete messages, and owns the scene, panels, and power overlay. */
class server {
public:
    /* Builds the display state: cursor, panels, power, wallpaper,
     * and hotkeys. Runs before the splash so its cost, dominated by
     * the wallpaper decode, never sits between Enter and the
     * desktop. Returns 0 or -1. */
    int init(presenter* pres, const stlxconf_t* conf);

    /* Binds the protocol socket and spawns the config's autostart
     * entries, the moment clients may exist. Returns 0 or -1. */
    int serve();

    void shutdown();

    /* Re-applies a freshly re-parsed config: panels, power, the
     * wallpaper, and the hotkeys rebuild, then the desktop repaints.
     * Autostart entries do not respawn. */
    void reload_config();

    /* Fills fds for one poll cycle: listen socket plus every client. */
    void collect_fds(std::vector<struct pollfd>& fds) const;

    /* Accepts new connections and pumps readable clients, then reaps
     * dead ones. Call after every poll wakeup. */
    void pump(const std::vector<struct pollfd>& fds);

    /* Whether damage, latches, configures, panel repaints, or a
     * running overlay animation wait for the paced tick. */
    bool compose_pending() const {
        return !m_damage.empty() || has_latch_work() ||
               has_configure_work() || m_panels->dirty() ||
               m_power->animating();
    }

    /* The bar clock's poll bound and tick, frozen while the overlay
     * dims the desktop. */
    int64_t clock_timeout_ns(uint64_t now_ns) const {
        return m_power->active() ? -1
                                 : m_panels->clock_timeout_ns(now_ns);
    }
    void clock_tick() {
        if (!m_power->active()) {
            m_panels->clock_tick();
        }
    }

    /* Latches pending commits into scene damage, composes exactly the
     * damaged regions into the presenter's target, and presents them.
     * Runs at the paced tick. */
    void compose_tick();

    /* Flushes every window's staged event batch. Call once per poll
     * wakeup after input, protocol, and compose work. */
    void flush_events();

    /* Input routing entry points, fed by the input layer. */
    void route_key(uint16_t usage, uint8_t hid_modifiers, bool down,
                   bool repeat);
    void route_pointer(int32_t x, int32_t y, uint16_t buttons,
                       uint16_t changed, int16_t wheel);

private:
    void accept_one();
    void pump_client(dm_client& c);

    void handle_message(dm_client& c, const swp_header& hdr,
                        const uint8_t* payload);

    /* window.cpp */
    void handle_create_window(dm_client& c, const uint8_t* payload);
    void handle_destroy_window(dm_client& c, const uint8_t* payload);
    void handle_set_window(dm_client& c, const uint8_t* payload);
    void handle_attach_buffer(dm_client& c, const uint8_t* payload);
    void handle_detach_buffer(dm_client& c, const uint8_t* payload);
    void handle_commit(dm_client& c, const uint8_t* payload);
    void destroy_window_tree(dm_client& c, uint32_t win_id);
    void latch_window(dm_client& c, dm_window& w);
    void flush_configures();
    void compose_rect(stlxgfx_surface_t* back, const damage_list::rect& r);

    void drop_client(dm_client& c);
    void spawn_shortcut(const char* path);
    int build_conf_state(dm_conf_state& out);
    void adopt_conf_state(dm_conf_state&& next);
    bool send_to(dm_client& c, uint16_t type,
                 const void* payload, uint32_t length);
    void flush_window_events(dm_client& c, dm_window& w);
    void flush_client(dm_client& c);

    /* events.cpp */
    dm_client* window_owner(const dm_window* w);
    dm_window* window_at(int32_t x, int32_t y, decor::zone* out_zone);
    void send_event(dm_window* w, const swp_event_rec& rec);
    void set_focus(dm_window* w);
    void forget_window(dm_window* w);

    /* scene.cpp */
    void scene_map(dm_window* w);
    void scene_raise(dm_window* w);
    void scene_damage_window(dm_window* w);

    bool has_latch_work() const;
    bool has_configure_work() const;

    int m_listen_fd = -1;
    presenter* m_presenter = nullptr;
    const stlxconf_t* m_conf = nullptr;
    std::vector<std::unique_ptr<dm_client>> m_clients;
    uint32_t m_window_count = 0;
    damage_list m_damage;

    /* The pre-scaled wallpaper, null when the config names none */
    stlxgfx_surface_t* m_wallpaper = nullptr;

    /* Exec shortcuts parsed from the config's key chords */
    std::vector<dm_hotkey> m_hotkeys;

    /* Mapped windows bottom to top. Popups sit directly above their
     * parent and travel with it on raise. */
    std::vector<dm_window*> m_zorder;

    /* Damage from recent frames, unioned in when the acquired target
     * is older than one present */
    static constexpr uint32_t DAMAGE_HISTORY = 3;
    damage_list m_history[DAMAGE_HISTORY];
    uint32_t m_history_head = 0;

    /* Input routing state, cleared by forget_window on death */
    dm_window* m_focus = nullptr;
    dm_window* m_hover = nullptr;
    dm_window* m_grab = nullptr;

    /* Active grabbing popup: it holds focus, and any press outside it
     * dismisses it and restores the focus it displaced */
    dm_window* m_grab_popup = nullptr;
    dm_window* m_focus_restore = nullptr;

    /* The compositor's own widget trees in the background band */
    std::unique_ptr<dm_panels> m_panels;

    /* The power overlay and its dock star */
    std::unique_ptr<dm_power> m_power;
    bool m_power_was_active = false;

    /* The pointer sprite: position, active shape, damage on change */
    cursor m_cursor;
    int32_t m_cursor_x = 0;
    int32_t m_cursor_y = 0;
    uint32_t m_cursor_shape = SWP_CURSOR_ARROW;

    void move_cursor(int32_t x, int32_t y, uint32_t shape);

    /* Title drag and close-control hover, decoration hit zones */
    dm_window* m_drag = nullptr;
    int32_t m_drag_dx = 0;
    int32_t m_drag_dy = 0;
    dm_window* m_close_hover = nullptr;
    dm_window* m_close_press = nullptr;

    /* Interactive resize: the grabbed window, which content edges the
     * pointer moves, the fixed opposite content corner, the rubber
     * band rect, and the sprite held for the whole drag */
    dm_window* m_resize = nullptr;
    uint8_t m_resize_edges = 0;
    int32_t m_resize_anchor_x = 0;
    int32_t m_resize_anchor_y = 0;
    damage_list::rect m_outline = {};
    uint32_t m_resize_shape = SWP_CURSOR_ARROW;

    void begin_resize(dm_window* w, decor::zone z, uint32_t shape);
    void update_resize(int32_t px, int32_t py);
    void damage_outline();

    /* One shared text clipboard, last writer wins */
    std::vector<char> m_clipboard;
};

#endif
