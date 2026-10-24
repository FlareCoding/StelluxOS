#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H

#include "common/types.h"
#include "drivers/graphics/gfxfb.h"
#include "fs/node.h"
#include "mm/device_backing.h"
#include "rc/strong_ref.h"
#include "sync/atomic.h"
#include "sync/mutex.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

// gfxfb keeps its state in a device node: gfxfb.cpp publishes one as /dev/gfxfb, and a test can publish a fresh one

namespace gfxfb {

// The events one client can hold unread, one per output, since an output holds one present at a time and a present
// fails while its client holds the event of the last one on its output
constexpr uint32_t CLIENT_EVENTS_MAX = GFXFB_OUTPUTS_MAX;

/**
 * The framebuffer the boot firmware left and its pixel layout, which
 * `GFXFB_GET_INFO` reports while the node shows its boot display.
 */
struct boot_framebuffer {
    buffer   pixels;
    uint16_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
};

/**
 * gfxfb's record of a buffer: a buffer an open file created, or a lit output's
 * base buffer. Each handle, each user mapping and the primary output hold a
 * reference, and each output holds one for its base buffer, one for the buffer
 * it shows and one for the buffer it holds for a present. After the last
 * reference goes, the reaper hands the buffer back to its display, which must
 * outlive the record. The record copies the buffer's fields when it takes the
 * buffer, so gfxfb reads only the copy. It lives on the privileged heap, since
 * mmap maps the physical address it holds, so its callers must run elevated.
 */
class buffer_record final : public mm::device_backing {
public:
    explicit buffer_record(display* owner) : m_display(owner) {}

    // Takes `buf` from the record's display, to hand back once the last reference goes
    void take(buffer* buf);

    const buffer& layout() const { return m_layout; }

    // The display's own object for the buffer, which gfxfb passes to the display's calls
    buffer* display_buffer() const { return m_buffer; }

private:
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void release_memory() override;

    display* m_display;
    buffer*  m_buffer = nullptr;
    buffer   m_layout = {};
};

/**
 * One open file of a gfxfb node: the buffers it created, each named by a
 * handle, the handles held for buffers that `GFXFB_CREATE_BUFFER` calls are
 * still creating, and the events of its presents' ends that it has not read.
 * `events_lock` is taken with interrupts disabled, since
 * `gfxfb_node::present_done()` may queue an event from an interrupt handler.
 * It lives on the privileged heap, from `open()` until the file's last
 * reference goes and the display has ended each present of it.
 */
struct client {
    rc::strong_ref<buffer_record> buffers[GFXFB_CLIENT_BUFFERS_MAX];   // handle h at index h - 1
    bool                          pending[GFXFB_CLIENT_BUFFERS_MAX] = {}; // true while a creation holds the handle

    sync::spinlock   events_lock;               // over the events, alone or inside the node's outputs lock
    gfxfb_event      events[CLIENT_EVENTS_MAX]; // `event_count` of them from `first_event` on, wrapping, oldest first
    uint32_t         first_event = 0;
    uint32_t         event_count = 0;
    sync::wait_queue event_waiters;             // woken as each present of the client ends
};

/**
 * The buffers one output of the shown display has as its base buffer, shows
 * and holds for a present, each through a reference to its record, the client
 * whose present the display has not ended, and whether a skipped
 * `GFXFB_FLUSH` left the base buffer stale. With the display reporting each
 * present's end as `gfxfb_node::present_done()` requires, no output scans out
 * memory its display has taken back.
 */
struct output_records {
    rc::strong_ref<buffer_record> base;   // recorded as gfxfb showed the display, null if dark then or memory ran out
    rc::strong_ref<buffer_record> shown;  // the last latched present's buffer, `base` before any latches
    rc::strong_ref<buffer_record> queued; // the buffer the output holds for a present, null while it holds none

    client*  presenter = nullptr;  // the client of the unended present, whose close waits for that present's end
    uint32_t presented_handle = 0; // the handle that present named, for its event
    bool     base_stale = false;   // set by a skipped FLUSH, so `base`'s next present passes the display no region
};

/**
 * gfxfb's display of the boot framebuffer: one output, lit and the boot
 * output, whose base buffer is the boot framebuffer. The output's mode is the
 * framebuffer's size with an unknown refresh rate. The display hardware scans
 * that memory out directly, so `flush()` has nothing to copy and returns `OK`.
 * It creates and presents no buffers, and keeps the boot framebuffer when
 * gfxfb hands it back.
 */
class boot_display : public display {
public:
    boot_display() = default;
    explicit boot_display(const boot_framebuffer& fb) : m_framebuffer(fb) {}

    uint32_t output_count() const override { return 1; }
    bool needs_flush() const override { return false; }
    int32_t read_output(uint32_t output, output_state& out) override;
    buffer* base_buffer(uint32_t output) override;
    int32_t flush(uint32_t output, const gfxfb_rect* rects, uint32_t count) override;
    int32_t create_buffer(uint32_t, uint32_t, uint32_t, buffer**) override { return ERR_UNSUPPORTED; }
    int32_t present(uint32_t, buffer*, const gfxfb_rect*, uint32_t) override { return ERR_UNSUPPORTED; }
    void release_buffer(buffer*) override {}

    const boot_framebuffer& framebuffer() const { return m_framebuffer; }

private:
    boot_framebuffer m_framebuffer = {};
};

/**
 * A gfxfb device node: the display it shows and that display's primary output,
 * whose base buffer userland maps. A node built from the boot framebuffer
 * shows it through its own boot display, and `register_display` puts a driver
 * display in the boot display's place. `GFXFB_FLUSH` hands its regions to the
 * shown display for the primary output unless the paragraph below says
 * otherwise. A display shown with every output dark, or when no memory is left
 * to record its lit outputs' base buffers, leaves the node with no primary
 * output while it is shown: `GFXFB_GET_INFO` and `GFXFB_FLUSH` refuse a
 * malformed request with `fs::ERR_INVAL`, then fail with `fs::ERR_AGAIN`
 * (FLUSH with `fs::ERR_IO` instead once the display has failed, as described
 * below) before GET_INFO writes its reply or FLUSH reads a rect, and mmap at
 * offset 0 is refused, while `GFXFB_GET_OUTPUT` still reports every output.
 * Each open file is a client, whose buffers mmap maps at the offsets their
 * creation returned, above the primary output's base buffer at offset 0. A
 * client controls the buffers from its first `GFXFB_CREATE_BUFFER` or
 * `GFXFB_PRESENT` that succeeds until it closes. Before control is checked, a
 * creation request fails with `fs::ERR_INVAL` when gfxfb cannot read it, when
 * its width or height is zero or when its reserved field is set, and a present
 * request fails with `fs::ERR_INVAL` when gfxfb cannot read it, when its
 * reserved field is set, when it names more than `GFXFB_MAX_FLUSH_RECTS`
 * regions, when it names an output of the display's output count or more and
 * when it names a handle its client does not hold, or otherwise with
 * `fs::ERR_AGAIN` for handle 0 on an output with no base buffer recorded: one
 * dark as the node showed its display, or any output when no memory was left
 * to record the lit outputs' base buffers. A present request past those checks
 * fails, still before control is checked, with `fs::ERR_NOMEM` when no memory
 * is left to copy its regions, and otherwise with `fs::ERR_INVAL` when gfxfb
 * cannot read them or one of them has a zero width or height or does not lie
 * wholly inside the buffer. Any other creation or present request fails with
 * `fs::ERR_BUSY` while another client controls the buffers or reserves
 * control, and otherwise reserves control for its client until the call
 * returns, so two clients never control the buffers at once. Each output holds
 * one present at a time, from just before gfxfb hands it to the display until
 * the display refuses it, reports it latched or reports the output dark, and
 * for good once the display reports it failed. A present request reaching an
 * output fails with `fs::ERR_IO` once the display has failed, as described
 * below, and otherwise with `fs::ERR_BUSY` when the output holds a present or
 * when the request's client holds the event of its last present there. Each
 * end the display reports reaches the presenting client as a `gfxfb_event`.
 * `read` returns as many of a client's events as fit, oldest first, in whole
 * records, and fails with `fs::ERR_INVAL` for a size below one record, with
 * `fs::ERR_AGAIN` when a non-blocking read finds none, and with `fs::ERR_INTR`
 * when a signal ends its wait for one. `poll` reports a client holding events
 * readable. A client that closes while a present of it has not ended waits for
 * that end, holding no lock. A closing client that controls the buffers then
 * drops its events and presents each output's recorded base buffer where the
 * output shows another buffer, naming no region. It ends its control only once
 * the display has ended those presents it accepted. An output with no base
 * buffer recorded, or whose base buffer's present is refused or ends dark or
 * failed, keeps showing the buffer it showed. A closing client is then freed
 * with the events it holds. Like every `fs::node`, a gfxfb node lives on the
 * privileged heap, so its callers must run elevated. devfs keeps a published
 * node for good, and a node is never freed once built, since the reaper may
 * still hand a base buffer back to its boot display.
 *
 * The shown display counts as failed once one of its calls returns ERR_DEVICE
 * or one of its presents ends failed, and from then on `GFXFB_FLUSH` fails
 * past its request checks with `fs::ERR_IO`, even with no primary output.
 * Otherwise, while the primary output shows a created buffer and holds no
 * present of its base buffer, a `GFXFB_FLUSH` past its request checks returns
 * `fs::OK` without reading a rect or calling the display, and marks the base
 * buffer stale, so that buffer's next present passes the display no region.
 */
class gfxfb_node : public fs::node {
public:
    /**
     * @brief Show the boot framebuffer through the node's boot display, with no primary output and an error logged when
     * no memory is left to record the framebuffer.
     * @param name Name of the node in devfs, copied.
     * @param boot_fb The boot framebuffer. Its memory must never be freed.
     */
    gfxfb_node(const char* name, const boot_framebuffer& boot_fb);

    /**
     * @brief Show `driver_display` from the start, for a registration that finds no /dev/gfxfb. A display with every
     * output dark leaves the node with no primary output, as does a lack of memory to record its lit outputs' base
     * buffers, which logs an error.
     * @param name Name of the node in devfs, copied.
     * @param driver_display The display. It must not be null and must never be freed.
     */
    gfxfb_node(const char* name, display* driver_display);

    /**
     * @brief Put a driver display in the boot display's place. It must be called before userland starts, as from a
     * driver's `attach()`, since the node reads its display and its primary output's record with no lock. A display is
     * refused once a program has opened the node, since that program's mappings would still reach the old base buffer.
     * @param driver_display The display. It must not be null and must never be freed.
     * @return OK, or ERR with a warning logged when the node already shows a driver display, when a program has opened
     * the node or when no output of `driver_display` is lit, or with an error logged when no memory is left to record
     * the base buffers of its lit outputs. The shown display stays in place on ERR.
     */
    int32_t register_display(display* driver_display);

    /**
     * @brief Record how the present the shown display accepted on `output` ended, and queue the end as an event for
     * the client that presented, waking the client's readers, pollers and closing task. On GFXFB_PRESENT_LATCHED the
     * output shows the present's buffer and drops the one it replaced, on GFXFB_PRESENT_DARK it drops the present's
     * buffer, and on GFXFB_PRESENT_FAILED it keeps both, since the failed hardware may still read either, and counts
     * the display as failed, so every later `GFXFB_FLUSH` past its request checks and every later present request
     * reaching an output fails with `fs::ERR_IO`. The display must call it exactly once per present its `present()`
     * accepted, possibly before that call returns, from its interrupt handler, or elevated from a task or a timer's
     * callback. The display must report GFXFB_PRESENT_LATCHED only once the hardware no longer reads the replaced
     * buffer and GFXFB_PRESENT_DARK only once the hardware can no longer latch the present. The call never sleeps or
     * calls the display.
     * @param output Index of the output the present was for.
     * @param result How the present ended.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void present_done(uint32_t output, const present_result& result);

    // True when the node shows a driver display, from construction or `register_display`, and false while it shows its
    // boot display
    bool has_driver_display() const { return m_display != &m_boot_display; }

    int32_t open(fs::file*, uint32_t flags) override;
    int32_t on_close(fs::file*) override;
    ssize_t read(fs::file*, void* buf, size_t count, uint32_t flags) override;
    uint32_t poll(fs::file*, sync::poll_table* pt) override;
    int32_t ioctl(fs::file*, uint32_t cmd, uint64_t arg) override;
    int32_t mmap(fs::file*, mm::mm_context* mm_ctx, uintptr_t addr,
                 size_t length, uint32_t prot, uint32_t map_flags,
                 uint64_t offset, uintptr_t* out_addr) override;

private:
    void init_locks();
    int32_t show(display* shown_display, uint32_t primary);
    void show_from_start(display* shown_display);
    int32_t get_info(uint64_t info_addr);
    int32_t get_output(uint64_t info_addr);
    int32_t flush(uint64_t args_addr);
    int32_t create_buffer(client& owner, uint64_t args_addr);
    int32_t create_buffer_under_handle(client& owner, gfxfb_buffer_args& args, uint64_t args_addr);
    int32_t destroy_buffer(client& owner, uint64_t handle);
    int32_t present(client& owner, uint64_t args_addr);
    rc::strong_ref<buffer_record> find_buffer_to_present(client& owner, const gfxfb_present_args& args);
    int32_t queue_present(
        client& owner,
        uint32_t output,
        uint32_t handle,
        const rc::strong_ref<buffer_record>& record,
        const gfxfb_rect* regions,
        uint32_t region_count
    );
    bool has_display_failed();
    void mark_failed_on_device_error(int32_t rc);
    bool holds_unended_present(const client& owner) const;
    void wait_for_unended_presents(client& owner);
    void give_outputs_back(client& owner);
    bool begin_controlled_call(client& caller);
    void end_controlled_call(bool succeeded);
    uint32_t take_handle(client& owner);
    void end_creation(client& owner, uint32_t handle, buffer_record* record);
    rc::strong_ref<buffer_record> find_buffer_at(client& owner, uint64_t offset, uint64_t& offset_in_buffer);

    boot_display                  m_boot_display;
    display*                      m_display = nullptr;
    uint32_t                      m_primary = 0;
    rc::strong_ref<buffer_record> m_primary_record; // null without a primary output, fixed once the node is opened
    sync::atomic<bool>            m_was_opened{false};

    sync::mutex m_clients_lock;          // over the clients' handles and control, never held across a display call
    client*     m_controller = nullptr;  // the client that controls the buffers or reserves control, or null
    bool        m_control_taken = false; // set once an admitted call succeeds, until `m_controller` closes
    uint32_t    m_controller_calls = 0;  // admitted calls of `m_controller` that have not ended

    sync::mutex    m_present_locks[GFXFB_OUTPUTS_MAX]; // so presents for one output reach the display one at a time
    sync::spinlock m_outputs_lock;                     // over the members below, taken with interrupts disabled
    output_records m_outputs[GFXFB_OUTPUTS_MAX];       // written unlocked only by `show()`, before the node is opened
    bool           m_display_failed = false;           // set for good by a display call's ERR_DEVICE or a failed end
};

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H
