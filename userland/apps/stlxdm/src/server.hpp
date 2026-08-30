#ifndef STLXDM_SERVER_HPP
#define STLXDM_SERVER_HPP

#include "damage.hpp"
#include "decor.hpp"
#include "presenter.hpp"

#include <stlxgfx/surface.h>
#include <stlxwin/proto.h>

#include <cstdint>
#include <memory>
#include <vector>

/* One attached shared-memory buffer, mapped for the window's lifetime
 * or until the client detaches it. */
struct dm_buffer {
    uint32_t  buf_id = 0;
    uint32_t  width = 0, height = 0;
    size_t    size = 0;
    uint32_t* pixels = nullptr;
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

    std::vector<std::unique_ptr<dm_window>> windows;

    explicit dm_client(int sock) : fd(sock) {}
};

/* The protocol server: listens, accepts, pumps client sockets, and
 * routes complete messages. Compose and input attach in later units. */
class server {
public:
    /* Binds and listens on the protocol socket. Returns 0 or -1. */
    int init(presenter* pres);
    void shutdown();

    /* Fills fds for one poll cycle: listen socket plus every client. */
    void collect_fds(std::vector<struct pollfd>& fds) const;

    /* Accepts new connections and pumps readable clients, then reaps
     * dead ones. Call after every poll wakeup. */
    void pump(const std::vector<struct pollfd>& fds);

    /* Whether damage is waiting for the next compose tick. */
    bool compose_pending() const { return !m_damage.empty() || has_latch_work(); }

    /* Latches pending commits into scene damage, composes exactly the
     * damaged regions into the presenter's target, and presents them.
     * Runs at the paced tick. */
    void compose_tick();

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
    void compose_rect(stlxgfx_surface_t* back, const damage_list::rect& r);

    void drop_client(dm_client& c);
    bool send_to(dm_client& c, uint16_t type,
                 const void* payload, uint32_t length);

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

    int m_listen_fd = -1;
    presenter* m_presenter = nullptr;
    std::vector<std::unique_ptr<dm_client>> m_clients;
    uint32_t m_window_count = 0;
    damage_list m_damage;

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

    /* Title drag and close-control hover, decoration hit zones */
    dm_window* m_drag = nullptr;
    int32_t m_drag_dx = 0;
    int32_t m_drag_dy = 0;
    dm_window* m_close_hover = nullptr;
    dm_window* m_close_press = nullptr;

    /* One shared text clipboard, last writer wins */
    std::vector<char> m_clipboard;
};

#endif
