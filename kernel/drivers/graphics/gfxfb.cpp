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

__PRIVILEGED_BSS static gfxfb_node* g_node;

// Trims `rect` to `fb` and returns false when nothing of it remains
static bool clip_rect_to_surface(gfxfb_rect& rect, const surface& fb) {
    if (rect.width == 0 || rect.height == 0 || rect.x >= fb.width || rect.y >= fb.height) {
        return false;
    }

    uint64_t max_width = fb.width - rect.x;
    if (rect.width > max_width) {
        rect.width = static_cast<uint32_t>(max_width);
    }

    uint64_t max_height = fb.height - rect.y;
    if (rect.height > max_height) {
        rect.height = static_cast<uint32_t>(max_height);
    }

    return true;
}

int32_t boot_display::flush(const gfxfb_rect*, uint32_t) {
    return OK;
}

gfxfb_node::gfxfb_node(const char* name, const surface& boot_fb)
    : fs::node(fs::node_type::char_device, nullptr, name) {
    show(boot_fb, &m_boot_display);
}

gfxfb_node::gfxfb_node(const char* name, const surface& fb, display* driver_display)
    : fs::node(fs::node_type::char_device, nullptr, name) {
    show(fb, driver_display);
}

int32_t gfxfb_node::register_display(const surface& fb, display* driver_display) {
    if (has_driver_display()) {
        log::warn("gfxfb: a driver display is already registered");
        return ERR;
    }

    show(fb, driver_display);

    return OK;
}

void gfxfb_node::show(const surface& fb, display* shown_display) {
    m_surface = fb;
    m_display = shown_display;
    m_size = surface_size();
}

int32_t gfxfb_node::ioctl(fs::file*, uint32_t cmd, uint64_t arg) {
    if (cmd == GFXFB_GET_INFO) {
        return get_info(arg);
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

    gfxfb_info info{};
    info.width       = m_surface.width;
    info.height      = m_surface.height;
    info.pitch       = m_surface.pitch;
    info.bpp         = m_surface.bpp;
    info.red_shift   = m_surface.red_shift;
    info.green_shift = m_surface.green_shift;
    info.blue_shift  = m_surface.blue_shift;
    info.flags       = has_driver_display() ? GFXFB_INFO_NEEDS_FLUSH : 0;
    info.size        = surface_size();

    int32_t rc = mm::uaccess::copy_to_user(
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

            if (clip_rect_to_surface(rect, m_surface)) {
                batch[kept++] = rect;
            }
        }

        if (kept > 0 && m_display->flush(batch, kept) != OK) {
            return fs::ERR_IO;
        }

        handled += batch_count;
    }

    return fs::OK;
}

int32_t gfxfb_node::mmap(fs::file*, mm::mm_context* mm_ctx, uintptr_t addr,
                         size_t length, uint32_t prot, uint32_t map_flags,
                         uint64_t offset, uintptr_t* out_addr) {
    size_t aligned_len = pmm::page_align_up(length);
    if (aligned_len < length) {
        log::error("gfxfb: mmap: aligned_len < length");
        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    if (offset + aligned_len < aligned_len) {
        log::error("gfxfb: mmap: offset overflow");
        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    size_t aligned_fb_size = pmm::page_align_up(surface_size());
    if (offset + aligned_len > aligned_fb_size) {
        log::error("gfxfb: mmap: offset+aligned_len=%lu > aligned_fb_size=%lu",
                   offset + aligned_len, aligned_fb_size);

        return mm::MM_CTX_ERR_INVALID_ARG;
    }

    uint32_t cache = m_surface.cacheable ? paging::PAGE_NORMAL : paging::PAGE_WC;
    log::info("gfxfb: mmap: phys=0x%lx len=%lu prot=%u cache=%u",
              m_surface.phys + offset, length, prot, cache);

    int32_t rc = mm::mm_context_map_device(
        mm_ctx,
        m_surface.phys + offset,
        length,
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

    const surface boot_surface = {
        .phys        = fb.fb_phys,
        .width       = fb.width,
        .height      = fb.height,
        .pitch       = fb.pitch,
        .bpp         = fb.bpp,
        .red_shift   = fb.red_mask_shift,
        .green_shift = fb.green_mask_shift,
        .blue_shift  = fb.blue_mask_shift,
        .cacheable   = false,
    };

    if (publish_node(heap::kalloc_new<gfxfb_node>(NODE_NAME, boot_surface)) != OK) {
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
__PRIVILEGED_CODE int32_t register_display(const surface& fb, display* driver_display) {
    if (!driver_display) {
        return ERR;
    }

    if (g_node) {
        if (g_node->register_display(fb, driver_display) != OK) {
            return ERR;
        }

        log::info("gfxfb: %lux%lu driver display replaced the boot framebuffer",
                  fb.width, fb.height);
        return OK;
    }

    if (publish_node(heap::kalloc_new<gfxfb_node>(NODE_NAME, fb, driver_display)) != OK) {
        return ERR;
    }

    log::info("gfxfb: %lux%lu driver display, registered /dev/gfxfb", fb.width, fb.height);

    return OK;
}

} // namespace gfxfb
