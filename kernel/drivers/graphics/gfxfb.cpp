#include "drivers/graphics/gfxfb.h"
#include "boot/boot_services.h"
#include "common/logging.h"
#include "drivers/graphics/gfxfb_node.h"
#include "fs/devfs/devfs.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "mm/uaccess.h"
#include "mm/vma.h"

namespace gfxfb {

// Rects copied in and handed to the display per call, small enough for the kernel stack
constexpr uint32_t RECTS_PER_DISPLAY_FLUSH = 16;

// /dev/gfxfb's name, given to the node and to devfs alike
constexpr const char* NODE_NAME = "gfxfb";

// The pixel layout of a driver display's buffers, which GFXFB_GET_INFO reports
constexpr uint16_t DRIVER_BPP         = 32;
constexpr uint8_t  DRIVER_RED_SHIFT   = 16;
constexpr uint8_t  DRIVER_GREEN_SHIFT = 8;
constexpr uint8_t  DRIVER_BLUE_SHIFT  = 0;

// What `find_primary` returns for a display with no output lit
constexpr uint32_t NO_OUTPUT = 0xFFFFFFFF;

__PRIVILEGED_BSS static gfxfb_node* g_node;

static uint64_t buffer_bytes(const buffer& buf) {
    return static_cast<uint64_t>(buf.pitch) * buf.height;
}

// Trims `rect` to `buf` and returns false when nothing of it remains
static bool clip_rect_to_buffer(gfxfb_rect& rect, const buffer& buf) {
    if (rect.width == 0 || rect.height == 0 || rect.x >= buf.width || rect.y >= buf.height) {
        return false;
    }

    uint32_t max_width = buf.width - rect.x;
    if (rect.width > max_width) {
        rect.width = max_width;
    }

    uint32_t max_height = buf.height - rect.y;
    if (rect.height > max_height) {
        rect.height = max_height;
    }

    return true;
}

// The lit boot output, else the lit output with the lowest index, else NO_OUTPUT
static uint32_t find_primary(display& shown_display) {
    uint32_t lowest_lit = NO_OUTPUT;

    for (uint32_t i = 0; i < shown_display.output_count(); i++) {
        output_state state = {};
        if (shown_display.read_output(i, state) != OK || !state.lit) {
            continue;
        }

        if (state.boot_output) {
            return i;
        }

        if (lowest_lit == NO_OUTPUT) {
            lowest_lit = i;
        }
    }

    return lowest_lit;
}

// The file layer's code for each result a display's `read_output()` and `flush()` return, and ERR_IO for any other
static int32_t to_fs_error(int32_t rc) {
    switch (rc) {
    case OK:              return fs::OK;
    case ERR_INVALID_ARG: return fs::ERR_INVAL;
    case ERR_DARK:        return fs::ERR_AGAIN;
    case ERR_DEVICE:      return fs::ERR_IO;
    default:              return fs::ERR_IO;
    }
}

int32_t boot_display::read_output(uint32_t output, output_state& out) {
    if (output != 0) {
        return ERR_INVALID_ARG;
    }

    out.lit = true;
    out.boot_output = true;

    // The boot framebuffer carries no refresh rate
    out.current = {
        .width              = m_framebuffer.pixels.width,
        .height             = m_framebuffer.pixels.height,
        .refresh_millihertz = 0,
    };

    return OK;
}

buffer* boot_display::base_buffer(uint32_t output) {
    return output == 0 ? &m_framebuffer.pixels : nullptr;
}

int32_t boot_display::flush(uint32_t, const gfxfb_rect*, uint32_t) {
    return OK;
}

gfxfb_node::gfxfb_node(const char* name, const boot_framebuffer& boot_fb)
    : fs::node(fs::node_type::char_device, nullptr, name), m_boot_display(boot_fb) {
    show(&m_boot_display, find_primary(m_boot_display));
}

gfxfb_node::gfxfb_node(const char* name, display* driver_display)
    : fs::node(fs::node_type::char_device, nullptr, name) {
    show(driver_display, find_primary(*driver_display));
}

int32_t gfxfb_node::register_display(display* driver_display) {
    if (has_driver_display()) {
        log::warn("gfxfb: a driver display is already registered");
        return ERR;
    }

    if (m_was_opened.load_relaxed()) {
        log::warn("gfxfb: refused a driver display, since a program has opened the device");
        return ERR;
    }

    uint32_t primary = find_primary(*driver_display);
    if (primary == NO_OUTPUT) {
        log::warn("gfxfb: refused a driver display with no output lit, keeping the boot framebuffer");
        return ERR;
    }

    show(driver_display, primary);

    return OK;
}

void gfxfb_node::show(display* shown_display, uint32_t primary) {
    m_display = shown_display;
    m_primary = primary;
    m_primary_buffer = primary == NO_OUTPUT ? nullptr : shown_display->base_buffer(primary);
    m_size = m_primary_buffer ? buffer_bytes(*m_primary_buffer) : 0;
}

int32_t gfxfb_node::open(fs::file*, uint32_t) {
    m_was_opened.store_relaxed(true);
    return fs::OK;
}

int32_t gfxfb_node::ioctl(fs::file*, uint32_t cmd, uint64_t arg) {
    if (cmd == GFXFB_GET_INFO) {
        return get_info(arg);
    }

    if (cmd == GFXFB_GET_OUTPUT) {
        return get_output(arg);
    }

    if (cmd == GFXFB_FLUSH) {
        return flush(arg);
    }

    return fs::ERR_NOSYS;
}

int32_t gfxfb_node::get_info(uint64_t info_addr) {
    if (info_addr == 0) {
        return fs::ERR_INVAL;
    }

    if (!m_primary_buffer) {
        return fs::ERR_AGAIN;
    }

    const buffer& base = *m_primary_buffer;

    gfxfb_info info{};
    info.width = base.width;
    info.height = base.height;
    info.pitch = base.pitch;
    info.flags = m_display->needs_flush() ? GFXFB_INFO_NEEDS_FLUSH : 0;
    info.size = buffer_bytes(base);

    if (has_driver_display()) {
        info.bpp = DRIVER_BPP;
        info.red_shift = DRIVER_RED_SHIFT;
        info.green_shift = DRIVER_GREEN_SHIFT;
        info.blue_shift = DRIVER_BLUE_SHIFT;
    } else {
        const boot_framebuffer& fb = m_boot_display.framebuffer();
        info.bpp = fb.bpp;
        info.red_shift = fb.red_shift;
        info.green_shift = fb.green_shift;
        info.blue_shift = fb.blue_shift;
    }

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(info_addr), &info, sizeof(info));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    return fs::OK;
}

int32_t gfxfb_node::get_output(uint64_t info_addr) {
    gfxfb_output_info info{};
    int32_t rc = mm::uaccess::copy_from_user(
        &info, reinterpret_cast<const void*>(info_addr), sizeof(info));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    output_state state = {};
    rc = m_display->read_output(info.output, state);
    if (rc != OK) {
        return to_fs_error(rc);
    }

    bool is_primary = m_primary_buffer != nullptr && info.output == m_primary;

    info.output_count = m_display->output_count();
    info.flags = (state.lit ? GFXFB_OUTPUT_LIT : 0) | (is_primary ? GFXFB_OUTPUT_PRIMARY : 0);
    info.reserved = 0;
    info.current = state.lit ? state.current : gfxfb_mode{};

    rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(info_addr), &info, sizeof(info));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    return fs::OK;
}

int32_t gfxfb_node::flush(uint64_t args_addr) {
    if (args_addr == 0) {
        return fs::ERR_INVAL;
    }

    gfxfb_flush_args args{};
    int32_t rc = mm::uaccess::copy_from_user(
        &args, reinterpret_cast<const void*>(args_addr), sizeof(args));

    if (rc != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    if (args.reserved != 0 || args.count > GFXFB_MAX_FLUSH_RECTS) {
        return fs::ERR_INVAL;
    }

    if (!m_primary_buffer) {
        return fs::ERR_AGAIN;
    }

    gfxfb_rect batch[RECTS_PER_DISPLAY_FLUSH];
    uint32_t handled = 0;

    while (handled < args.count) {
        uint32_t batch_count = args.count - handled;
        if (batch_count > RECTS_PER_DISPLAY_FLUSH) {
            batch_count = RECTS_PER_DISPLAY_FLUSH;
        }

        uint64_t batch_addr = args.rects + static_cast<uint64_t>(handled) * sizeof(gfxfb_rect);
        rc = mm::uaccess::copy_from_user(
            batch, reinterpret_cast<const void*>(batch_addr), batch_count * sizeof(gfxfb_rect));

        if (rc != mm::uaccess::OK) {
            return fs::ERR_INVAL;
        }

        uint32_t kept = 0;
        for (uint32_t i = 0; i < batch_count; i++) {
            gfxfb_rect rect = batch[i];

            if (clip_rect_to_buffer(rect, *m_primary_buffer)) {
                batch[kept++] = rect;
            }
        }

        if (kept > 0) {
            rc = m_display->flush(m_primary, batch, kept);
            if (rc != OK) {
                return to_fs_error(rc);
            }
        }

        handled += batch_count;
    }

    return fs::OK;
}

int32_t gfxfb_node::mmap(fs::file*, mm::mm_context* mm_ctx, uintptr_t addr,
                         size_t length, uint32_t prot, uint32_t map_flags,
                         uint64_t offset, uintptr_t* out_addr) {
    if (!m_primary_buffer) {
        log::error("gfxfb: mmap: no output is lit");
        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    size_t aligned_len = pmm::page_align_up(length);
    if (aligned_len < length) {
        log::error("gfxfb: mmap: aligned_len < length");
        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    if (offset + aligned_len < aligned_len) {
        log::error("gfxfb: mmap: offset overflow");
        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    const buffer& base = *m_primary_buffer;
    size_t aligned_fb_size = pmm::page_align_up(buffer_bytes(base));
    if (offset + aligned_len > aligned_fb_size) {
        log::error("gfxfb: mmap: offset+aligned_len=%lu > aligned_fb_size=%lu",
                   offset + aligned_len, aligned_fb_size);

        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    uint32_t cache = base.cacheable ? paging::PAGE_NORMAL : paging::PAGE_WC;
    log::info("gfxfb: mmap: phys=0x%lx len=%lu prot=%u cache=%u",
              base.phys + offset, length, prot, cache);

    // The base buffers gfxfb maps, the boot framebuffer and a registered display's, live as long as the kernel, so
    // mapping one passes no owner
    int32_t rc = mm::mm_context_map_device(
        mm_ctx,
        base.phys + offset,
        length,
        nullptr,
        0,
        prot,
        cache,
        map_flags,
        addr,
        out_addr
    );

    if (rc != mm::MM_CTX_OK) {
        log::error("gfxfb: mmap: mm_context_map_device failed rc=%d", rc);
    }

    return rc;
}

/**
 * @brief Publish `node` as /dev/gfxfb and keep it as gfxfb's node, freeing it when devfs refuses it.
 * @param node A node named `NODE_NAME` from `heap::kalloc_new`, or nullptr when the allocation failed.
 * @return OK, or ERR with an error logged when `node` is null or devfs refuses it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t publish_node(gfxfb_node* node) {
    if (!node) {
        log::error("gfxfb: failed to allocate gfxfb_node");
        return ERR;
    }

    int32_t rc = devfs::add_char_device(NODE_NAME, node);
    if (rc != devfs::OK) {
        log::error("gfxfb: failed to register /dev/gfxfb");
        heap::kfree_delete(node);
        return ERR;
    }

    g_node = node;

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    auto& fb = g_boot_info.framebuffer;
    if (fb.fb_phys == 0) {
        log::info("gfxfb: no framebuffer available");
        return OK;
    }

    const boot_framebuffer boot_fb = {
        .pixels = {
            .width     = static_cast<uint32_t>(fb.width),
            .height    = static_cast<uint32_t>(fb.height),
            .pitch     = static_cast<uint32_t>(fb.pitch),
            .phys      = fb.fb_phys,
            .cacheable = false,
        },
        .bpp         = fb.bpp,
        .red_shift   = fb.red_mask_shift,
        .green_shift = fb.green_mask_shift,
        .blue_shift  = fb.blue_mask_shift,
    };

    if (publish_node(heap::kalloc_new<gfxfb_node>(NODE_NAME, boot_fb)) != OK) {
        return ERR;
    }

    log::info("gfxfb: %lux%lu %ubpp phys=0x%lx size=%lu, registered /dev/gfxfb",
              fb.width, fb.height, static_cast<unsigned int>(fb.bpp),
              fb.fb_phys, fb.pitch * fb.height);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool has_driver_display() {
    return g_node && g_node->has_driver_display();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool has_boot_display() {
    return g_node && !g_node->has_driver_display();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_display(display* driver_display) {
    if (!driver_display) {
        return ERR;
    }

    if (g_node) {
        if (g_node->register_display(driver_display) != OK) {
            return ERR;
        }

        log::info("gfxfb: a driver display replaced the boot framebuffer");
        return OK;
    }

    if (publish_node(heap::kalloc_new<gfxfb_node>(NODE_NAME, driver_display)) != OK) {
        return ERR;
    }

    log::info("gfxfb: registered /dev/gfxfb with a driver display");

    return OK;
}

} // namespace gfxfb
