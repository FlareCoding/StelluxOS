#include "drivers/graphics/gfxfb.h"
#include "boot/boot_services.h"
#include "common/logging.h"
#include "fs/devfs/devfs.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "mm/uaccess.h"
#include "mm/vma.h"

namespace gfxfb {

namespace {

class gfxfb_node : public fs::node {
public:
    gfxfb_node(fs::instance* fs, const char* name,
               uint64_t phys, uint64_t w, uint64_t h, uint64_t p,
               uint16_t bpp, uint8_t r, uint8_t g, uint8_t b)
        : fs::node(fs::node_type::char_device, fs, name)
        , m_phys(phys), m_width(w), m_height(h), m_pitch(p)
        , m_bpp(bpp), m_red_shift(r), m_green_shift(g), m_blue_shift(b)
        , m_fb_size(p * h) {
        m_size = m_fb_size;
    }

    int32_t ioctl(fs::file*, uint32_t cmd, uint64_t arg) override {
        if (cmd != GFXFB_GET_INFO) {
            return fs::ERR_NOSYS;
        }
        if (arg == 0) {
            return fs::ERR_INVAL;
        }

        gfxfb_info info{};
        info.width       = m_width;
        info.height      = m_height;
        info.pitch       = m_pitch;
        info.bpp         = m_bpp;
        info.red_shift   = m_red_shift;
        info.green_shift = m_green_shift;
        info.blue_shift  = m_blue_shift;
        info.size        = m_fb_size;

        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &info, sizeof(info));
        if (rc != mm::uaccess::OK) {
            return fs::ERR_INVAL;
        }
        return fs::OK;
    }

    int32_t mmap(fs::file*, mm::mm_context* mm_ctx, uintptr_t addr,
                 size_t length, uint32_t prot, uint32_t map_flags,
                 uint64_t offset, uintptr_t* out_addr) override {
        size_t aligned_len = pmm::page_align_up(length);
        if (aligned_len < length) {
            return mm::MM_CTX_ERR_INVALID_ARG;
        }
        if (offset + aligned_len < aligned_len) {
            return mm::MM_CTX_ERR_INVALID_ARG;
        }
        if (offset + aligned_len > m_fb_size) {
            return mm::MM_CTX_ERR_INVALID_ARG;
        }

        return mm::mm_context_map_device(
            mm_ctx,
            m_phys + offset,
            length,
            prot,
            paging::PAGE_WC,
            map_flags,
            addr,
            out_addr
        );
    }

    int32_t getattr(fs::vattr* attr) override {
        if (!attr) return fs::ERR_INVAL;
        attr->type = fs::node_type::char_device;
        attr->size = m_fb_size;
        return fs::OK;
    }

private:
    uint64_t m_phys;
    uint64_t m_width;
    uint64_t m_height;
    uint64_t m_pitch;
    uint16_t m_bpp;
    uint8_t  m_red_shift;
    uint8_t  m_green_shift;
    uint8_t  m_blue_shift;
    uint64_t m_fb_size;
};

} // namespace

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    auto& fb = g_boot_info.framebuffer;
    if (fb.fb_phys == 0) {
        log::info("gfxfb: no framebuffer available");
        return OK;
    }

    void* mem = heap::kzalloc(sizeof(gfxfb_node));
    if (!mem) {
        log::error("gfxfb: failed to allocate gfxfb_node");
        return ERR;
    }

    auto* node = new (mem) gfxfb_node(
        nullptr, "gfxfb",
        fb.fb_phys, fb.width, fb.height, fb.pitch,
        fb.bpp, fb.red_mask_shift, fb.green_mask_shift, fb.blue_mask_shift
    );

    int32_t rc = devfs::add_char_device("gfxfb", node);
    if (rc != devfs::OK) {
        log::error("gfxfb: failed to register /dev/gfxfb");
        node->~gfxfb_node();
        heap::kfree(mem);
        return ERR;
    }

    log::info("gfxfb: %lux%lu %ubpp, registered /dev/gfxfb",
              fb.width, fb.height, static_cast<unsigned int>(fb.bpp));
    return OK;
}

} // namespace gfxfb
