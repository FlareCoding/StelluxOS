#ifndef STLXDM_SERVER_HPP
#define STLXDM_SERVER_HPP

#include "animation.hpp"
#include "cursor.hpp"
#include "damage.hpp"
#include "decor.hpp"
#include "display.hpp"
#include "frame_stats.hpp"
#include "gpu_compositor.hpp"
#include "layout.hpp"
#include "layout_store.hpp"
#include "panels.hpp"
#include "placement.hpp"
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

/* Frames of damage an output keeps for targets older than one present */
constexpr uint32_t DM_DAMAGE_HISTORY = 3;

/* One parsed shortcut: what it runs, a program with its args or one of stlxdm's own actions, and the usage that
 * fires it while exactly its modifier bits are held */
struct dm_hotkey {
    uint8_t mods = 0;
    uint16_t usage = 0;
    stlxconf_action_kind_t kind = STLXCONF_ACTION_EXEC;
    const char* path = nullptr;
    const char* args = nullptr;
};

/* Config-derived state built as a unit and adopted only when every
 * piece came up, so a failed reload keeps the running desktop. The
 * backdrop is the main output's wallpaper with the panels painted in,
 * and the frost is made from the wallpaper alone. */
struct dm_conf_state {
    std::unique_ptr<dm_panels> panels;
    std::unique_ptr<dm_power> power;
    stlxgfx_surface_t* backdrop = nullptr;
    std::unique_ptr<backdrop_picture> gpu_backdrop;  /* the backdrop under the GPU compositor, `backdrop` then null */
    stlxgfx_surface_t* frost = nullptr;
    std::unique_ptr<frost_picture> gpu_frost;        /* the frost under the GPU compositor, `frost` then null */
    std::unique_ptr<picture> gpu_capture;            /* the overlay's collapse backdrop under the GPU compositor */
    std::vector<dm_hotkey> hotkeys;
};

/* An output's own pictures under the GPU compositor: its backdrop and
 * frost on any output but the main one, the draws above the windows
 * beneath the power overlay's dim and above it, and what each of its
 * composes reuses */
struct dm_output_pictures {
    std::unique_ptr<backdrop_picture>                backdrop;
    std::unique_ptr<frost_picture>                   frost;
    std::unique_ptr<top_picture>                     lower_top;
    std::unique_ptr<top_picture>                     upper_top;
    std::unique_ptr<gpu_compositor::compose_buffers> compose;
    layer_scene                                      scene;
    std::vector<damage_list::rect>                   rects;

    /* GPU frames and missed vertical blanks since the last frame statistics line, cleared with the pictures */
    frame_stats                                      stats;

    /* In comparison mode: each GPU frame's rows read back at the output
     * buffer's pitch, the CPU compositor's frame, the regions where the two
     * may round apart, and how many frames were compared */
    std::unique_ptr<picture>                         readback;
    std::unique_ptr<picture>                         comparison;
    std::vector<damage_list::rect>                   rounding;
    uint64_t                                         compared = 0;
};

/* One output as the compositor sees it, kept by its index and cleared
 * while the output is not served. */
struct dm_output {
    presenter*         pres = nullptr;    /* the display's, null while not served */
    damage_list::rect  place;             /* desktop coordinates */

    /* Damage in the output's own pixels since its last compose, and that
     * of its recent frames for targets older than one present */
    damage_list        damage;
    damage_list        history[DM_DAMAGE_HISTORY];
    uint32_t           history_head = 0;

    /* The wallpaper or flat color at the output's size and its frost,
     * null on the main output, when they could not be built and under
     * the GPU compositor */
    stlxgfx_surface_t* backdrop = nullptr;
    stlxgfx_surface_t* frost = nullptr;
    bool               backdrop_failed = false;       /* its last build failed */

    bool               cursor_overlaps = false;       /* the pointer's picture overlaps the output */
    const uint32_t*    cursor_picture = nullptr;      /* the picture its hardware cursor shows */
    bool               cursor_in_frames = false;      /* drawn into frames since its cursor could not show it */
    bool               cursor_move_pending = false;   /* a move waits for the flush */

    dm_output_pictures pictures;                      /* empty under the CPU compositor */
};

/* The wallpaper for one refresh of the backdrops, decoded at its first
 * use and freed with this, so a refresh decodes it at most once. */
class dm_wallpaper {
public:
    explicit dm_wallpaper(const stlxconf_t& conf) : m_conf(conf) {}
    ~dm_wallpaper();
    dm_wallpaper(const dm_wallpaper&) = delete;
    dm_wallpaper& operator=(const dm_wallpaper&) = delete;

    /* The decoded image, null when none is configured or it fails to
     * load. */
    const stlxgfx_surface_t* image();

private:
    const stlxconf_t&  m_conf;
    stlxgfx_surface_t* m_image = nullptr;
    bool               m_decoded = false;
};

/* One attached buffer. A GPU buffer holds its timelines and its newest
 * commit's points, except that a release-only one, whose commits are
 * released at once, holds only its release timeline. Every other buffer
 * keeps its read-only handle or descriptor, its mapping when it is
 * mappable, and under the GPU compositor its imports, until it is
 * dropped. A GPU buffer that is not mappable is release-only whenever
 * the GPU compositor does not compose alone. */
struct dm_buffer {
    uint32_t  buf_id = 0;
    uint32_t  width = 0, height = 0;
    uint32_t  pitch = 0;            /* bytes from one row to the next */
    void*     mapping = nullptr;
    size_t    size = 0;             /* the buffer's bytes */
    uint32_t* pixels = nullptr;     /* the picture's first pixel, null without a mapping */
    uint64_t  offset = 0;           /* bytes from the buffer's start to the picture */
    uint64_t  gpu_address = 0;      /* the GPU compositor's address of the picture, 0 without an import */
    int       shm = -1;             /* a shared memory buffer's object, opened read-only */
    int       gpu_buffer = -1;      /* the read-only handle that a GPU_BUFFER_IMPORT of the buffer gave */
    uint32_t  placement = 0;        /* the GPU_PLACEMENT_* that import gave */

    /* The GPU compositor's import, which `gpu_address` lies in */
    std::unique_ptr<window_import> import;

    bool      is_gpu = false;
    bool      is_premultiplied = false;  /* the pixels are premultiplied ARGB8888, not XRGB8888 */
    bool      is_release_only = false;
    bool      is_detached = false;  /* shown until the next latch drops it */
    int       acquire = -1;         /* read handle of the acquire timeline */
    int       release = -1;         /* write handle of the release timeline */
    uint64_t  acquire_point = 0;
    uint64_t  latch_point = 0;      /* 0 when the commit named none */
    uint64_t  release_point = 0;    /* the newest named, 0 before any commit */
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
    uint32_t output = 0;        /* a toplevel's: the member holding the largest part of its frame */

    /* The output and position a toplevel's last WINDOW_OUTPUT named */
    bool     output_sent = false;
    uint32_t sent_output = 0;
    int32_t  sent_x = 0, sent_y = 0;

    window_placement placement;  /* a toplevel's, at its defaults on popups */

    uint32_t flags = 0;
    uint32_t popup_flags = 0;
    char     title[SWP_TITLE_MAX] = {};

    std::vector<dm_buffer> buffers;
    int32_t  pending = -1;      /* buffers index awaiting latch */
    int32_t  current = -1;      /* buffers index on screen */
    uint32_t sent_serial = 0;
    uint32_t acked_serial = 0;
    uint32_t cursor = 0;        /* swp_cursor, shown once the sprite lands */
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

    /* Damage of the commits since the last latch, buffer coordinates,
     * unless the whole buffer changed. */
    swp_rect pending_damage[SWP_COMMIT_MAX_RECTS];
    uint32_t pending_damage_count = 0;
    bool     pending_damage_whole = false;

    /* When a pending GPU commit arrived, and when its acquire point is
     * next due a query once its first second of per-tick queries ends */
    uint64_t pending_since_ns = 0;
    uint64_t next_query_ns = 0;

    /* Its decoration as either compositor blends it, drawn while that
     * compositor composes the window */
    frame_picture frame;

    /* The twin that the GPU compositor reads in place of the current buffer */
    std::unique_ptr<window_twin> twin;
    bool                         has_twin_failed = false;  /* no twin fits until the buffer's shape changes */
};

/* One window control of one window, none when `window` is null */
struct dm_control {
    dm_window*  window = nullptr;
    decor::zone zone = decor::zone::none;

    bool operator==(const dm_control&) const = default;
};

/* A dismissed window's release-only record, kept until the client
 * detaches it or destroys the window, as commits may still name it */
struct dm_dismissed_buffer {
    uint32_t  win_id = 0;
    dm_buffer buffer;
};

/* One connected application. Owns the socket, the partial-message
 * assembly buffer, and its windows. */
struct dm_client {
    int      fd = -1;
    bool     hello_done = false;
    bool     dead = false;
    bool     may_attach_gpu = false;  /* its first GPU_INFO offered GPU buffers */
    char     app_id[SWP_APP_ID_MAX] = {};

    uint8_t  rd_buf[SWP_MAX_MSG_SIZE];
    uint32_t rd_have = 0;

    /* Handles received for ATTACH_GPU_BUFFER and CLIPBOARD_SET messages not
     * yet handled, oldest first. One read reaches into at most two of them. */
    static constexpr uint32_t QUEUED_HANDLES_MAX = 2 * SWP_GPU_HANDLES;
    int      queued_handles[QUEUED_HANDLES_MAX] = {};
    uint32_t queued_handle_count = 0;

    /* Outbound bytes the socket would not take, bounded, flushed on
     * POLLOUT. The tail marker enables in-place motion coalescing. */
    std::vector<uint8_t> out_q;
    size_t   q_motion_off = SIZE_MAX;
    uint32_t q_motion_win = 0;

    /* CLIPBOARD_GETs not yet answered. An answer's handle must ride its
     * first byte, so answers go out only once out_q is empty. */
    uint32_t clipboard_answers_owed = 0;

    std::vector<std::unique_ptr<dm_window>> windows;

    /* Windows the display manager destroyed on its own, whose messages
     * in flight are ignored until the client learns and destroys them */
    std::vector<uint32_t> dismissed_windows;

    std::vector<dm_dismissed_buffer> dismissed_buffers;

    /* One bit per output whose MODES the client asked for */
    uint32_t mode_watches = 0;

    uint32_t outputs_generation = 0;  /* the generation of the last OUTPUTS it was sent */

    explicit dm_client(int sock) : fd(sock) {}
};

/* A client's SET_MODE or KEEP_MODE, kept until the main loop has read
 * the wakeup's messages, since a request waits for a relight */
struct dm_mode_message {
    dm_client* client = nullptr;
    uint16_t   type = 0;        /* SWP_MSG_SET_MODE or SWP_MSG_KEEP_MODE */
    uint32_t   output = 0;
    uint32_t   value = 0;       /* SET_MODE's flags or KEEP_MODE's keep */
    swp_mode   mode = {};
};

/* A mode request waiting to be made or being followed, from a
 * client's message or from stlxdm itself */
struct dm_mode_request {
    dm_client* client = nullptr;  /* null when nobody waits for the answer */
    uint32_t   output = 0;
    uint32_t   outcome = 0;       /* the SWP_MODE_* it answers */
    int32_t    result = 0;        /* GFXFB_SET_MODE's, once made */
    swp_mode   mode = {};
    swp_mode   previous = {};     /* the output's mode before a client's request */
};

/* The one trial at a time, on any output, from the answer to its lit
 * request until it is kept, set back or ended by an output event */
struct dm_trial {
    bool       running = false;
    dm_client* client = nullptr;  /* the asking client, valid only while running */
    uint32_t   output = 0;
    swp_mode   mode = {};
    swp_mode   previous = {};
    uint64_t   deadline_ns = 0;
};

/* A client's SET_LAYOUT, applied once the wakeup's messages are read */
struct dm_layout_request {
    dm_client*     client = nullptr;
    swp_set_layout request = {};
};

/* A LAYOUT_RESULT or MODE_RESULT built when its request was handled and
 * sent at the end of the wakeup, after any OUTPUTS */
struct dm_answer {
    dm_client*        client = nullptr;
    uint16_t          type = 0;      /* SWP_MSG_LAYOUT_RESULT or SWP_MSG_MODE_RESULT */
    swp_layout_result layout = {};   /* its generation filled as it is sent */
    swp_mode_result   mode = {};
};

/* The protocol server: listens, accepts, pumps client sockets, routes
 * complete messages, and owns the layout, the scene, panels, and power
 * overlay. */
class server {
public:
    /* Builds the desktop's state over the outputs `disp` serves: the
     * layout, cursor, panels, power, wallpaper, hotkeys, GPU device, and
     * the GPU compositor unless the config asks for the CPU or it cannot be
     * made. Runs before the splash so its cost, dominated by the wallpaper
     * decode, never sits between Enter and the desktop. Returns 0 or -1. */
    int init(display* disp, const stlxconf_t* conf);

    /* Moves every served output to the GPU compositor `choose_compositor()`
     * made, with a GPU presenter and its pictures holding the backdrops, or
     * composes on the CPU, dropping the GPU compositor with the window
     * imports and printing why, when any of them cannot be made. Does
     * nothing without a GPU compositor. Runs after the splash and before
     * `serve()`, and at a reload's switch to the GPU. */
    void switch_to_gpu();

    /* Once the retired GPU compositor's last compose ended or its device is
     * lost, destroys it and takes a reload's switch to the GPU that waited
     * for it, or ends its retirement when composing stopped instead,
     * reading its timeline without waiting. Runs at the end of each switch
     * to the CPU and at each turn of the loop. */
    void reap_retired_compositor();

    /* The poll bound while a GPU compositor is retired, so its timeline is
     * read at least that often, or -1 while none is retired */
    int64_t retired_timeout_ns() const;

    /* Raises the handle limit, binds the protocol socket and spawns the
     * config's autostart entries, the moment clients may exist. Returns 0
     * or -1. */
    int serve();

    const layout& desktop() const { return m_layout; }

    /* The main output's presenter, null while it is not served. */
    presenter* main_presenter() const { return m_outputs[m_layout.main_output()].pres; }

    /* Shows the pointer on the hardware cursor of every served output
     * its picture overlaps, where that output can show the shape, else in
     * its frames, printing which, and hides it on the other served
     * outputs. Runs after the splash, which has no pointer, after each
     * switch of compositors, and from `follow_outputs()`. */
    void show_cursor();

    /* Whether a pointer move waits for `output`'s hardware cursor. */
    bool cursor_move_pending(uint32_t output) const { return m_outputs[output].cursor_move_pending; }

    /* Sends `output`'s hardware cursor the waiting move, at the pointer's
     * latest position: one call however many motions came. A refused
     * move puts the pointer into that output's frames. */
    void flush_cursor(uint32_t output);

    void shutdown();

    /* Re-applies a freshly re-parsed config: unless composing stopped, a
     * change of `compositor` to cpu leaves the GPU compositor and one to
     * auto tries it, then panels, power, the wallpaper, and the hotkeys
     * rebuild, and the desktop repaints. Autostart entries do not respawn,
     * and `compare_frames` takes effect at the next switch to the GPU. */
    void reload_config();

    /* Places the members again after output events or a mode request's
     * answer, first leaving the GPU compositor, as `switch_to_cpu` does,
     * when an output's GPU presenter could not be made, rebuilding the
     * panels, power and backdrop when the main output changed or its size
     * differs from the backdrop's (keeping the running ones on failure),
     * and repaints every served output whole. A change of places moves
     * windows and popups with their outputs, and it or a rebuild keeps them
     * in reach. Returns the pointer, moved with its output or, once that
     * left, to the main output's center, shown and routed as a motion with
     * `buttons` held. */
    desktop_point follow_outputs(desktop_point pointer, uint16_t buttons);

    /* Decides the wakeup's layout requests in arrival order, applying and
     * storing each that holds and staging its answer, and returns the
     * pointer, which a change of places moves with windows and popups as
     * `follow_outputs()` does. */
    desktop_point apply_layout_requests(desktop_point pointer, uint16_t buttons);

    /* Sends the current OUTPUTS to each client past its handshake that
     * holds an older one, a WINDOW_OUTPUT for each mapped toplevel whose
     * output or position changed since its last one, the position only
     * once a drag or resize of the window ends, then the wakeup's layout
     * and mode results in the order their requests were handled. */
    void flush_outputs();

    /* Fills fds for one poll cycle: listen socket plus every client. */
    void collect_fds(std::vector<struct pollfd>& fds) const;

    /* Accepts new connections and pumps readable clients, then reaps
     * dead ones. Call after every poll wakeup. */
    void pump(const std::vector<struct pollfd>& fds);

    /* Moves the desktop's damage into the damage of every served output
     * it overlaps, in that output's pixels. */
    void spread_damage();

    /* Whether damage, latches or configures wait for `output`'s paced
     * tick, as of the last `spread_damage()`. On the main output, panel
     * repaints and a running overlay animation wait for it too. */
    bool compose_pending(uint32_t output) const;

    /* The poll bound until the next query of a pending GPU commit older
     * than a second, which the tick no longer runs for, or -1 if none. */
    int64_t latch_timeout_ns(uint64_t now_ns) const;

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

    /* Shows the focused window's app on the bar and the running apps' ticks
     * on the dock. Call once per wakeup, after input and client messages are
     * handled. */
    void sync_shell();

    /* Whether a power overlay phase or hold is in flight. */
    bool power_animating() const { return m_power->animating(); }

    /* Advances the power overlay and, once its collapse ends, runs a
     * confirmed action, as the main output's `compose_tick()` does while
     * it is served. */
    void power_tick();

    /* Latches the pending commits `output`'s composes take, composes
     * exactly its damaged regions into its presenter's target, and
     * presents them. Runs at the output's paced tick. */
    void compose_tick(uint32_t output);

    /* Flushes every window's staged event batch. Call once per poll
     * wakeup after input, protocol, and compose work. */
    void flush_events();

    /* Answers the messages queued ahead of the next request, then makes
     * it and reads its output events. Returns false if none waits. */
    bool start_mode_request();

    /* Answers the started request, starting a lit trial's timer, or
     * leaves the previous mode or the default as the next request. The
     * started request's output events must be followed first. */
    void finish_mode_request();

    /* Sends the watchers of `outputs`, one bit each, their MODES, and
     * ends a trial whose output went dark or to another mode. */
    void outputs_changed(uint32_t outputs);

    /* The trial timer's poll bound and tick, which leaves the trial's
     * set-back waiting once it runs out. */
    int64_t mode_timeout_ns(uint64_t now) const;
    void mode_tick(uint64_t now);

    /* Input routing entry points, fed by the input layer and, for the
     * pointer, by `adopt_layout()`. */
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
    void handle_attach_gpu_buffer(dm_client& c, const uint8_t* payload);
    void handle_detach_buffer(dm_client& c, const uint8_t* payload);
    void handle_commit(dm_client& c, const uint8_t* payload);
    void handle_commit_gpu(dm_client& c, const uint8_t* payload);
    void handle_activate_window(dm_client& c, const uint8_t* payload);
    void replace_pending(dm_client& c, dm_window& w, int32_t index);
    void destroy_window_tree(dm_client& c, uint32_t win_id);
    void dismiss_window_tree(dm_client& c, uint32_t win_id);
    void note_dismissed_tree(dm_client& c, uint32_t win_id);
    void release_held_buffers(dm_client& c, const dm_window& w);
    void drop_buffer(dm_client& c, dm_window& w, size_t index);
    void keep_release_only_record(dm_client& c, dm_window& w, size_t index);
    void signal_release(dm_client& c, const dm_buffer& b, uint64_t point);
    void release_picture(dm_client& c, uint32_t win_id, const dm_buffer& b);
    void stop_taking_gpu_buffers();
    void send_gpu_info(uint32_t flags);
    void follow_reader_in_gpu_info();
    uint32_t accepting_gpu_flags() const;
    bool gpu_reads_buffers() const { return composes_on_gpu() && !m_compares_frames; }
    void release_unmappable_buffers();
    bool import_shared_memory(dm_buffer& b);
    bool import_attached_buffers();
    void drop_window_imports();
    void switch_to_cpu_on_failed_gpu();
    bool is_acquire_reached(dm_client& c, dm_window& w, uint64_t now);
    void latch_window(dm_client& c, dm_window& w);
    void flush_configures();
    decor::chrome_state chrome_of(const dm_window& w) const;
    picture_maker& frame_maker();
    void flush_panels();
    bool compose_on_cpu(dm_output& out, const presenter::target& t, const damage_list& effective);
    bool compose_on_gpu(dm_output& out, const presenter::target& t, const damage_list& effective);
    bool compose_capture(dm_output& out);
    bool switch_to_cpu_on_misfit_backdrop(dm_output& out);
    bool switch_to_cpu_on_failed_compose(compose_result result);
    void note_gpu_frame(dm_output& out);
    void follow_compose_fault();
    void follow_compositor();
    void switch_to_gpu_at_reload();
    void switch_to_cpu(const char* reason);
    gpu_compositor* retired_compositor() const;
    void compose_rect(dm_output& out, stlxgfx_surface_t* back, const damage_list::rect& r);
    bool composes_on_gpu() const { return m_gpu_composes && !m_gpu_stopped; }
    backdrop_picture& gpu_backdrop_of(dm_output& out);
    frost_picture* gpu_frost_of(dm_output& out);
    const stlxgfx_surface_t* frost_of(dm_output& out);
    bool draw_pictures(dm_output& out, const std::vector<damage_list::rect>& rects, bool is_capture);
    void draw_top_pictures(dm_output& out, const std::vector<damage_list::rect>& rects, bool is_capture);
    window_twin* twin_of(dm_window& w, const dm_buffer& b);
    void compose_collapse(dm_output& out);
    void draw_collapse_frame(dm_output& out, stlxgfx_surface_t* back);
    bool collapse_on_cpu(dm_output& out, const presenter::target& t);
    bool collapse_on_gpu(dm_output& out, const presenter::target& t);
    void compare_with_cpu(dm_output& out, uint32_t pitch, bool is_collapse);
    void rounding_regions(dm_output& out);
    void advance_power();
    const dm_window& toplevel_of(const dm_client& c, const dm_window& w) const;
    uint32_t latch_output(const dm_client& c, const dm_window& w) const;
    void update_window_output(dm_window& w);

    void drop_client(dm_client& c);
    void run_shortcut(const dm_hotkey& hotkey);
    void open_power();
    int build_conf_state(dm_conf_state& out, dm_wallpaper& wallpaper);
    void adopt_conf_state(dm_conf_state&& next);
    void refresh_backdrops(bool rebuild_all, dm_wallpaper& wallpaper);
    std::unique_ptr<backdrop_picture> copy_backdrop(const stlxgfx_surface_t* backdrop, uint32_t width, uint32_t height);
    std::unique_ptr<frost_picture> copy_frost(const stlxgfx_surface_t* frost);
    bool make_output_pictures(uint32_t output);
    void make_served_pictures();
    uint32_t gather_members(layout::member* members) const;
    const layout::arrangement* stored_layout(const layout::member* members, uint32_t count, layout::arrangement& out);
    uint32_t apply_layout_request(const swp_set_layout& request);
    desktop_point adopt_layout(const layout& before, desktop_point pointer, uint16_t buttons);
    void describe_outputs(swp_outputs& out) const;
    void update_generation();
    void flush_window_outputs();
    void sync_outputs();
    void move_window_tree(dm_client& c, dm_window* w, int32_t dx, int32_t dy);
    void move_windows_with_outputs(const layout& before);
    void keep_windows_in_reach();
    bool send_to(dm_client& c, uint16_t type,
                 const void* payload, uint32_t length,
                 uint16_t header_flags = 0);
    void flush_window_events(dm_client& c, dm_window& w);
    void flush_client(dm_client& c);
    void set_clipboard(dm_client& c, const uint8_t* payload);
    void copy_text(const std::string& text);
    bool send_clipboard_answer(dm_client& c);
    void send_clipboard_answers(dm_client& c);

    /* events.cpp */
    dm_client* window_owner(const dm_window* w);
    dm_window* popup_parent(const dm_window& w);
    dm_window* window_at(int32_t x, int32_t y, decor::zone* out_zone);
    void send_event(dm_window* w, const swp_event_rec& rec);
    void set_focus(dm_window* w);
    void forget_window(dm_window* w);

    /* scene.cpp */
    void scene_map(dm_window* w);
    void scene_raise(dm_window* w);
    void scene_damage_window(dm_window* w);
    void scene_hide(dm_window* top);
    void scene_show(dm_window* top);

    /* placement.cpp */
    void place_window(dm_window* w, const damage_list::rect& content);
    damage_list::rect state_rect(const dm_window& w) const;
    void set_window_state(dm_window* w, window_state state);
    void toggle_maximized(dm_window* w);
    bool record_title_press(dm_window* w, int32_t x, int32_t y);
    bool restore_dragged_window(int32_t x, int32_t y);
    void refit_placed_windows();
    bool is_hidden(const dm_window& top) const;
    void minimize_window(dm_window* w);
    void unminimize_window(dm_window* w);
    bool has_launcher(const dm_client& c) const;
    damage_list::rect launcher_tile_of(const dm_client& c) const;
    bool unminimize_app_window(const stlxconf_pin_t& pin);
    void unminimize_windows_without_launcher();
    window_animation* find_animation(const dm_window* w);
    bool animate_window(dm_window* w, window_transition transition, const damage_list::rect& from,
                        const damage_list::rect& to);
    void advance_animations();
    void forget_animation(const dm_window* w);
    const frame_picture* animation_frame(window_animation& a, picture_maker& maker);
    void damage_painted(const dm_window& w, const damage_list::rect& frame);
    void compose_animations(const decor::canvas& canvas, const damage_list::rect& r, const stlxgfx_surface_t* frost);
    bool add_animations(dm_output& out);

    /* modes.cpp */
    bool keeps_place_for_mode_request(uint32_t output) const;
    void handle_get_modes(dm_client& c, const uint8_t* payload);
    void queue_mode_message(dm_client& c, uint16_t type, const uint8_t* payload);
    void take_mode_message(const dm_mode_message& m);
    void make_mode_request(dm_mode_request& request);
    void answer_mode(dm_client* c, uint32_t output, int32_t result, uint32_t outcome);
    void send_modes(dm_client& c, uint32_t output);
    void end_trial_with_set_back(dm_client* answer_to);
    void forget_mode_client(dm_client& c);

    bool has_latch_work(uint32_t output) const;
    bool has_configure_work() const;

    int m_listen_fd = -1;
    display* m_display = nullptr;
    const stlxconf_t* m_conf = nullptr;

    /* The glass level and tint the material tables and every frost are
     * built for, which a failed reload restores */
    stlxconf_glass_level_t m_glass_level = STLXCONF_GLASS_LEVEL_GLASS;
    stlxconf_glass_tint_t  m_glass_tint = STLXCONF_GLASS_TINT_WARM;

    /* The GPU compositor, null while the CPU composes, and the one the CPU
     * took over from until it is reaped, both before the clients and
     * outputs since their pictures must drop first */
    std::unique_ptr<gpu_compositor> m_gpu_compositor;
    std::unique_ptr<gpu_compositor> m_retired_compositor;

    /* The maker of frame pictures while no GPU compositor exists, declared
     * before the clients since their frame pictures must drop first */
    heap_picture_maker m_heap_picture_maker;

    std::vector<std::unique_ptr<dm_client>> m_clients;

    /* The outputs' places, each output's record by its index, and the
     * count of windows opened on each output, which sets their cascade */
    layout m_layout;
    dm_output m_outputs[gfxfb::GFXFB_OUTPUTS_MAX];
    uint32_t m_window_count[gfxfb::GFXFB_OUTPUTS_MAX] = {};

    /* The stored layouts, and the monitors the layout was last built for */
    layout_store         m_store;
    layout_store::member m_monitors[layout::MEMBERS_MAX];
    uint32_t             m_monitor_count = 0;

    /* The newest OUTPUTS, its generation raised from 1 at every change, and
     * the wakeup's layout requests and the answers staged for its end */
    swp_outputs                    m_desktop = {};
    std::vector<dm_layout_request> m_layout_requests;
    std::vector<dm_answer>         m_answers;

    /* Desktop damage not yet spread to the outputs */
    damage_list m_damage;

    /* The GPU whose buffers clients may attach, -1 without one, and the
     * GPU_INFO every client gets after HELLO_REPLY */
    int m_gpu_fd = -1;
    swp_gpu_info m_gpu_info = {};

    /* Makes the GPU compositor unless the config asks for the CPU or no
     * GPU device opened, printing why the CPU composes otherwise */
    void choose_compositor();

    /* The main output's backdrop: wallpaper or flat color with the
     * panels painted in, in `m_gpu_backdrop` under the GPU compositor */
    stlxgfx_surface_t* m_backdrop = nullptr;
    std::unique_ptr<backdrop_picture> m_gpu_backdrop;

    /* The frost of the main output's wallpaper, in `m_gpu_frost` under
     * the GPU compositor */
    stlxgfx_surface_t* m_frost = nullptr;
    std::unique_ptr<frost_picture> m_gpu_frost;

    /* The power overlay's collapse backdrop under the GPU compositor, at
     * the main backdrop's size, which the overlay views and a
     * compose writes */
    std::unique_ptr<picture> m_gpu_capture;

    /* Whether the GPU compositor holds the served outputs' presenters and
     * pictures, and whether its composes stopped with no CPU takeover */
    bool m_gpu_composes = false;
    bool m_gpu_stopped = false;

    /* Whether each GPU frame is compared with the CPU compositor's, from
     * the config at the last switch to the GPU */
    bool m_compares_frames = false;

    /* The config's last `compose_fault`, so only a reload that changes it
     * from none fails a GPU compose */
    stlxconf_compose_fault_t m_compose_fault = STLXCONF_COMPOSE_FAULT_NONE;

    /* The config's last `compositor`, so only a reload that changes it
     * switches, and whether a switch to the GPU waits for a retired one */
    stlxconf_compositor_t m_compositor = STLXCONF_COMPOSITOR_AUTO;
    bool                  m_gpu_switch_waits = false;

    /* Exec shortcuts parsed from the config's key chords */
    std::vector<dm_hotkey> m_hotkeys;

    /* Mapped windows bottom to top. Popups sit directly above their
     * parent and travel with it on raise. */
    std::vector<dm_window*> m_zorder;

    /* Toplevels out of the scene while they show resampled between two
     * places, above every window in the scene, in the order they started */
    std::vector<window_animation> m_animations;

    /* Input routing state, cleared by forget_window on death */
    dm_window* m_focus = nullptr;
    dm_window* m_hover = nullptr;
    dm_window* m_grab = nullptr;

    /* The active grabbing popup, and the window the first menu of its chain
     * took the focus from, refocused when the chain closes holding the focus */
    dm_window* m_grab_popup = nullptr;
    dm_window* m_focus_restore = nullptr;

    /* When the user last pressed a key or button, which a window's request to come forward must follow closely */
    uint64_t m_last_press_ns = 0;

    /* The system figures the shell shows, kept across rebuilds of the panels */
    stats_sampler m_stats;

    /* The compositor's own widget trees in the background band */
    std::unique_ptr<dm_panels> m_panels;

    /* The power overlay */
    std::unique_ptr<dm_power> m_power;
    bool m_power_was_active = false;

    /* The pointer sprite, its position in desktop coordinates and its
     * active shape, shown on each output as its `dm_output` says */
    cursor m_cursor;
    int32_t m_cursor_x = 0;
    int32_t m_cursor_y = 0;
    uint32_t m_cursor_shape = SWP_CURSOR_ARROW;

    void move_cursor(int32_t x, int32_t y, uint32_t shape);
    bool show_hardware_cursor(dm_output& out);
    damage_list::rect cursor_bounds() const;
    void damage_cursor();

    /* State of the decoration hit zones: the title drag, the window control
     * under the pointer and the one a press holds */
    dm_window* m_drag = nullptr;
    int32_t m_drag_dx = 0;
    int32_t m_drag_dy = 0;
    dm_control m_control_hover;
    dm_control m_control_press;

    /* The last press on a title bar, where a title drag starts, and its
     * window until a second press near it soon after makes a double click */
    dm_window* m_title_press = nullptr;
    uint64_t   m_title_press_ns = 0;
    int32_t    m_title_press_x = 0;
    int32_t    m_title_press_y = 0;

    void move_window(dm_window* w, int32_t dx, int32_t dy);
    void damage_controls(const dm_control& control);
    void release_control_press();
    void act_on_control(dm_window* w, decor::zone control);

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

    /* One shared text clipboard, last writer wins: its text's length and
     * the shared memory object holding it, -1 while empty */
    uint64_t m_clipboard_len = 0;
    int      m_clipboard_object = -1;

    /* Mode messages in arrival order, and the next request to make, a
     * message's or stlxdm's own, made before any later message */
    std::vector<dm_mode_message> m_mode_messages;
    dm_mode_request m_next_request;
    bool m_next_request_waiting = false;

    dm_mode_request m_request;  /* the one being followed */
    bool m_request_following = false;  /* from making `m_request` until finishing it */
    dm_trial m_trial;
};

#endif
