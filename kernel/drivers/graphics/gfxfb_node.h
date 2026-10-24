#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H

#include "common/types.h"
#include "drivers/graphics/gfxfb.h"
#include "fs/node.h"
#include "mm/device_backing.h"
#include "rc/strong_ref.h"
#include "sync/atomic.h"
#include "sync/mutex.h"

// gfxfb keeps its state in a device node: gfxfb.cpp publishes one as /dev/gfxfb, and a test can publish a fresh one

namespace gfxfb {

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
 * gfxfb's record of a buffer userland can map: a buffer an open file created,
 * or the primary output's base buffer. Each handle, the primary output and
 * each user mapping hold a reference, and after the last goes the reaper hands
 * the buffer back to its display, which must outlive the record. The record
 * copies the buffer's fields when it takes the buffer, so gfxfb never reads
 * the display's object again. It lives on the privileged heap, since mmap maps
 * the physical address it holds, so its callers must run elevated.
 */
class buffer_record final : public mm::device_backing {
public:
    explicit buffer_record(display* owner) : m_display(owner) {}

    // Takes `buf` from the record's display, to hand back once the last reference goes
    void take(buffer* buf);

    const buffer& layout() const { return m_layout; }

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
 * handle, and the handles held for buffers that `GFXFB_CREATE_BUFFER` calls
 * are still creating. It lives on the privileged heap, from `open()` until the
 * file's last reference goes.
 */
struct client {
    rc::strong_ref<buffer_record> buffers[GFXFB_CLIENT_BUFFERS_MAX];   // handle h at index h - 1
    bool                          pending[GFXFB_CLIENT_BUFFERS_MAX] = {}; // true while a creation holds the handle
};

/**
 * gfxfb's display of the boot framebuffer: one output, lit and the boot
 * output, whose base buffer is the boot framebuffer. The output's mode is the
 * framebuffer's size with an unknown refresh rate. The display hardware scans
 * that memory out directly, so `flush()` has nothing to copy and returns `OK`.
 * It creates no buffers, and keeps the boot framebuffer when gfxfb hands it
 * back.
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
 * shown display for the primary output. A display shown with every output
 * dark, or when no memory is left to record its primary output's base buffer,
 * leaves the node with no primary output while it is shown: `GFXFB_GET_INFO`
 * and `GFXFB_FLUSH` refuse a malformed request with `fs::ERR_INVAL`, then fail
 * with `fs::ERR_AGAIN` before GET_INFO writes its reply or FLUSH reads a rect,
 * and mmap at offset 0 is refused, while `GFXFB_GET_OUTPUT` still reports
 * every output. Each open file is a client, whose buffers mmap maps at the
 * offsets their creation returned, above the primary output's base buffer at
 * offset 0. A client controls the buffers from its first `GFXFB_CREATE_BUFFER`
 * that succeeds until it closes. Before control is checked, a creation request
 * fails with `fs::ERR_INVAL` when gfxfb cannot read it, when its width or
 * height is zero or when its reserved field is set. Any other creation request
 * fails with `fs::ERR_BUSY` while another client controls the buffers or
 * reserves control, and otherwise reserves control for its client until the
 * call returns, so two clients never control the buffers at once. Like every
 * `fs::node`, it lives on the privileged heap, so its callers must run
 * elevated. devfs keeps a published node for good, and a node is never freed
 * once built, since the reaper may still hand a base buffer back to its boot
 * display.
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
     * output dark leaves the node with no primary output, as does a lack of memory to record the primary output's base
     * buffer, which logs an error.
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
     * the primary output's base buffer. The shown display stays in place on ERR.
     */
    int32_t register_display(display* driver_display);

    // True when the node shows a driver display, from construction or `register_display`, and false while it shows its
    // boot display
    bool has_driver_display() const { return m_display != &m_boot_display; }

    int32_t open(fs::file*, uint32_t flags) override;
    int32_t on_close(fs::file*) override;
    int32_t ioctl(fs::file*, uint32_t cmd, uint64_t arg) override;
    int32_t mmap(fs::file*, mm::mm_context* mm_ctx, uintptr_t addr,
                 size_t length, uint32_t prot, uint32_t map_flags,
                 uint64_t offset, uintptr_t* out_addr) override;

private:
    int32_t show(display* shown_display, uint32_t primary);
    void show_from_start(display* shown_display);
    int32_t get_info(uint64_t info_addr);
    int32_t get_output(uint64_t info_addr);
    int32_t flush(uint64_t args_addr);
    int32_t create_buffer(client& owner, uint64_t args_addr);
    int32_t create_buffer_under_handle(client& owner, gfxfb_buffer_args& args, uint64_t args_addr);
    int32_t destroy_buffer(client& owner, uint64_t handle);
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
};

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H
