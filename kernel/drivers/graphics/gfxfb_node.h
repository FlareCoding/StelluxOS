#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H

#include "common/types.h"
#include "drivers/graphics/gfxfb.h"
#include "fs/node.h"

// gfxfb keeps its state in a device node: gfxfb.cpp publishes one as /dev/gfxfb, and a test can publish a fresh one

namespace gfxfb {

/**
 * gfxfb's display of the boot framebuffer. The boot framebuffer holds the
 * pixels the screen shows, so `flush()` has nothing to copy and returns `OK`.
 */
class boot_display : public display {
public:
    int32_t flush(const gfxfb_rect* rects, uint32_t count) override;
};

/**
 * A gfxfb device node: the surface userland maps and the display that shows it.
 * A node built from the boot framebuffer shows it through its own boot display,
 * and `register_display` puts a driver display in the boot display's place.
 * `GFXFB_FLUSH` hands its regions to whichever display the node shows. Like
 * every `fs::node`, it lives on the privileged heap, so its callers must run
 * elevated. devfs keeps a published node for good.
 */
class gfxfb_node : public fs::node {
public:
    /**
     * @brief Show the boot framebuffer through the node's boot display.
     * @param name Name of the node in devfs, copied.
     * @param boot_fb The boot framebuffer. Its memory must never be freed.
     */
    gfxfb_node(const char* name, const surface& boot_fb);

    /**
     * @brief Show `driver_display` from the start, for a registration that finds no /dev/gfxfb.
     * @param name Name of the node in devfs, copied.
     * @param fb Surface userland maps and draws into. Its memory must never be freed.
     * @param driver_display Display that flushes `fb`. It must not be null and must never be freed.
     */
    gfxfb_node(const char* name, const surface& fb, display* driver_display);

    /**
     * @brief Put a driver display in the boot display's place. It must be called before any program opens the node, as
     * from a driver's `attach()`, since the node reads its surface and display with no lock, and a mapping made earlier
     * would still reach the old surface.
     * @param fb Surface userland maps and draws into. Its memory must never be freed.
     * @param driver_display Display that flushes `fb`. It must not be null and must never be freed.
     * @return OK, or ERR with a warning logged when the node already shows a driver display, which stays in place.
     */
    int32_t register_display(const surface& fb, display* driver_display);

    // True when the node shows a driver display, from construction or `register_display`
    bool has_driver_display() const { return m_display != &m_boot_display; }

    int32_t ioctl(fs::file*, uint32_t cmd, uint64_t arg) override;
    int32_t mmap(fs::file*, mm::mm_context* mm_ctx, uintptr_t addr,
                 size_t length, uint32_t prot, uint32_t map_flags,
                 uint64_t offset, uintptr_t* out_addr) override;

private:
    void show(const surface& fb, display* shown_display);
    int32_t get_info(uint64_t info_addr);
    int32_t flush(uint64_t args_addr);
    uint64_t surface_size() const { return m_surface.pitch * m_surface.height; }

    boot_display m_boot_display;
    surface      m_surface = {};
    display*     m_display = nullptr;
};

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_NODE_H
