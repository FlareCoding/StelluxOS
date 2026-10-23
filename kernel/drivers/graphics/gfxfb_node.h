#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H

#include "common/types.h"
#include "drivers/graphics/gfxfb.h"
#include "fs/node.h"
#include "sync/atomic.h"

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
 * gfxfb's display of the boot framebuffer: one output, lit and the boot
 * output, whose base buffer is the boot framebuffer. The output's mode is the
 * framebuffer's size with an unknown refresh rate. The display hardware scans
 * that memory out directly, so `flush()` has nothing to copy and returns `OK`.
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

    const boot_framebuffer& framebuffer() const { return m_framebuffer; }

private:
    boot_framebuffer m_framebuffer = {};
};

/**
 * A gfxfb device node: the display it shows and that display's primary output,
 * whose base buffer userland maps. A node built from the boot framebuffer
 * shows it through its own boot display, and `register_display` puts a driver
 * display in the boot display's place. `GFXFB_FLUSH` hands its regions to the
 * shown display for the primary output. A display shown with every output dark
 * leaves the node with no primary output for its life: `GFXFB_GET_INFO` and
 * `GFXFB_FLUSH` refuse a malformed request with `fs::ERR_INVAL`, then fail
 * with `fs::ERR_AGAIN` before GET_INFO writes its reply or FLUSH reads a rect,
 * and mmap is refused, while `GFXFB_GET_OUTPUT` still reports every output.
 * Like every `fs::node`, it lives on the privileged heap, so its callers must
 * run elevated. devfs keeps a published node for good.
 */
class gfxfb_node : public fs::node {
public:
    /**
     * @brief Show the boot framebuffer through the node's boot display.
     * @param name Name of the node in devfs, copied.
     * @param boot_fb The boot framebuffer. Its memory must never be freed.
     */
    gfxfb_node(const char* name, const boot_framebuffer& boot_fb);

    /**
     * @brief Show `driver_display` from the start, for a registration that finds no /dev/gfxfb. A display with every
     * output dark leaves the node with no primary output.
     * @param name Name of the node in devfs, copied.
     * @param driver_display The display. It must not be null and must never be freed.
     */
    gfxfb_node(const char* name, display* driver_display);

    /**
     * @brief Put a driver display in the boot display's place. It must be called before userland starts, as from a
     * driver's `attach()`, since the node reads its display with no lock. A display is refused once a program has
     * opened the node, since that program's mappings would still reach the old base buffer.
     * @param driver_display The display. It must not be null and must never be freed.
     * @return OK, or ERR with a warning logged when the node already shows a driver display, when a program has opened
     * the node, or when no output of `driver_display` is lit. The shown display stays in place on ERR.
     */
    int32_t register_display(display* driver_display);

    // True when the node shows a driver display, from construction or `register_display`, and false while it shows its
    // boot display
    bool has_driver_display() const { return m_display != &m_boot_display; }

    int32_t open(fs::file*, uint32_t flags) override;
    int32_t ioctl(fs::file*, uint32_t cmd, uint64_t arg) override;
    int32_t mmap(fs::file*, mm::mm_context* mm_ctx, uintptr_t addr,
                 size_t length, uint32_t prot, uint32_t map_flags,
                 uint64_t offset, uintptr_t* out_addr) override;

private:
    void show(display* shown_display, uint32_t primary);
    int32_t get_info(uint64_t info_addr);
    int32_t get_output(uint64_t info_addr);
    int32_t flush(uint64_t args_addr);

    boot_display       m_boot_display;
    display*           m_display = nullptr;
    uint32_t           m_primary = 0;
    buffer*            m_primary_buffer = nullptr; // null while the display has no primary output
    sync::atomic<bool> m_was_opened{false};
};

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H
