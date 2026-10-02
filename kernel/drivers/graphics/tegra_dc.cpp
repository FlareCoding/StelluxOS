#include "drivers/graphics/tegra_dc.h"

#if defined(STLX_PLATFORM_JETSON_NANO)

#include "boot/boot_services.h"
#include "common/logging.h"
#include "hw/mmio.h"
#include "mm/paging_types.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

namespace tegra_dc {

namespace {

// Clock and reset controller: DISP1 clock enable / reset bits
constexpr uint64_t CAR_PHYS = 0x60006000;
constexpr size_t   CAR_SIZE = 0x1000;
constexpr uint32_t CAR_RST_DEVICES_L = 0x004;
constexpr uint32_t CAR_CLK_OUT_ENB_L = 0x010;
constexpr uint32_t CLK_L_DISP1 = 1u << 27;

// Display controller A, which drives HDMI on the Nano devkit
constexpr uint64_t DC_PHYS = 0x54200000;
constexpr size_t   DC_SIZE = 0x40000;

// DC register word offsets
constexpr uint32_t DC_CMD_DISPLAY_COMMAND       = 0x032;
constexpr uint32_t DC_CMD_STATE_ACCESS          = 0x040;
constexpr uint32_t DC_CMD_DISPLAY_WINDOW_HEADER = 0x042;
constexpr uint32_t DC_WIN_WIN_OPTIONS           = 0x700;
constexpr uint32_t DC_WIN_COLOR_DEPTH           = 0x703;
constexpr uint32_t DC_WIN_SIZE                  = 0x705;
constexpr uint32_t DC_WIN_PRESCALED_SIZE        = 0x706;
constexpr uint32_t DC_WIN_LINE_STRIDE           = 0x70a;
constexpr uint32_t DC_WINBUF_START_ADDR         = 0x800;
constexpr uint32_t DC_WINBUF_ADDR_H_OFFSET      = 0x806;
constexpr uint32_t DC_WINBUF_ADDR_V_OFFSET      = 0x808;

constexpr uint32_t DISP_CTRL_MODE_MASK   = 3u << 5;
constexpr uint32_t STATE_ACCESS_READ_MUX = 1u << 0; // read active, not assembly, state
constexpr uint32_t WINDOW_A_SELECT       = 1u << 4; // B and C follow at bits 5 and 6
constexpr uint32_t WIN_ENABLE            = 1u << 30;

constexpr uint32_t WIN_COLOR_DEPTH_B8G8R8A8 = 12;
constexpr uint32_t WIN_COLOR_DEPTH_R8G8B8A8 = 13;

constexpr uint32_t WINDOW_COUNT = 3;

struct window_state {
    uint32_t options;
    uint32_t depth;
    uint32_t size;
    uint32_t prescaled;
    uint32_t stride;
    uint32_t start;
    uint32_t h_offset;
    uint32_t v_offset;
};

__PRIVILEGED_CODE uint32_t dc_read(uintptr_t dc, uint32_t reg) {
    return mmio::read32(dc + reg * 4);
}

__PRIVILEGED_CODE void dc_write(uintptr_t dc, uint32_t reg, uint32_t val) {
    mmio::write32(dc + reg * 4, val);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uintptr_t map_regs(uint64_t phys, size_t size, uintptr_t& base) {
    uintptr_t va = 0;
    if (vmm::map_device(static_cast<pmm::phys_addr_t>(phys), size,
                        paging::PAGE_KERNEL_RW, base, va) != vmm::OK) {
        return 0;
    }
    return va;
}

/**
 * The display power partition state can't be checked directly: the Nano's
 * PMC is secure-only ("secure-pmc"), so non-secure reads return zero. The
 * firmware only ungates the DISP1 clock and releases its reset after powering
 * the partition, so clock-on plus reset-released implies the DC is reachable.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool display_powered() {
    uintptr_t car_base = 0;
    uintptr_t car = map_regs(CAR_PHYS, CAR_SIZE, car_base);
    if (car == 0) {
        return false;
    }

    uint32_t clk = mmio::read32(car + CAR_CLK_OUT_ENB_L);
    uint32_t rst = mmio::read32(car + CAR_RST_DEVICES_L);
    (void)vmm::free(car_base);

    bool clocked  = (clk & CLK_L_DISP1) != 0;
    bool in_reset = (rst & CLK_L_DISP1) != 0;

    log::info("tegra_dc: DISP1 clock=%s reset=%s",
              clocked ? "on" : "off", in_reset ? "held" : "released");

    return clocked && !in_reset;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool find_active_window(uintptr_t dc, window_state& out) {
    uint32_t saved_access = dc_read(dc, DC_CMD_STATE_ACCESS);
    uint32_t saved_header = dc_read(dc, DC_CMD_DISPLAY_WINDOW_HEADER);
    dc_write(dc, DC_CMD_STATE_ACCESS, saved_access | STATE_ACCESS_READ_MUX);

    bool found = false;
    for (uint32_t w = 0; w < WINDOW_COUNT; w++) {
        dc_write(dc, DC_CMD_DISPLAY_WINDOW_HEADER, WINDOW_A_SELECT << w);

        window_state s{};
        s.options   = dc_read(dc, DC_WIN_WIN_OPTIONS);
        s.depth     = dc_read(dc, DC_WIN_COLOR_DEPTH);
        s.size      = dc_read(dc, DC_WIN_SIZE);
        s.prescaled = dc_read(dc, DC_WIN_PRESCALED_SIZE);
        s.stride    = dc_read(dc, DC_WIN_LINE_STRIDE);
        s.start     = dc_read(dc, DC_WINBUF_START_ADDR);
        s.h_offset  = dc_read(dc, DC_WINBUF_ADDR_H_OFFSET);
        s.v_offset  = dc_read(dc, DC_WINBUF_ADDR_V_OFFSET);

        log::info("tegra_dc: window %c options=0x%08x depth=%u size=0x%08x "
                  "prescaled=0x%08x stride=0x%08x start=0x%08x offset=%u,%u",
                  static_cast<char>('A' + w), s.options, s.depth, s.size,
                  s.prescaled, s.stride, s.start, s.h_offset, s.v_offset);

        if (!found && (s.options & WIN_ENABLE) != 0 && s.start != 0) {
            out = s;
            found = true;
        }
    }

    dc_write(dc, DC_CMD_DISPLAY_WINDOW_HEADER, saved_header);
    dc_write(dc, DC_CMD_STATE_ACCESS, saved_access);
    return found;
}

/**
 * Every page of the buffer must be memory the PMM will never allocate:
 * either outside tracked RAM or marked reserved from the firmware memory map.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool buffer_is_reserved(uint64_t phys, uint64_t size) {
    uint64_t start = pmm::page_align_down(phys);
    uint64_t end = pmm::page_align_up(phys + size);
    for (uint64_t addr = start; addr < end; addr += pmm::PAGE_SIZE) {
        pmm::page_frame_descriptor* pf = pmm::get_page_frame(addr);
        if (pf && !pf->is_reserved()) {
            log::error("tegra_dc: buffer page 0x%lx is allocatable RAM", addr);
            return false;
        }
    }
    return true;
}

/**
 * Parse a decimal or 0x-prefixed hex number, advancing `p`.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool parse_number(const char*& p, const char* end, uint64_t& out) {
    uint64_t base = 10;
    if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }

    const char* digits = p;
    uint64_t value = 0;
    while (p < end) {
        char c = *p;
        uint64_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = static_cast<uint64_t>(c - 'A' + 10);
        else break;
        value = value * base + d;
        p++;
    }

    if (p == digits) return false;
    out = value;
    return true;
}

} // namespace

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool boot_fbmem(uint64_t* out_base, uint64_t* out_size) {
    if (!g_boot_info.dtb_ptr || g_boot_info.dtb_size == 0) {
        return false;
    }

    static constexpr char KEY[] = "tegra_fbmem=";
    constexpr size_t KEY_LEN = sizeof(KEY) - 1;

    const char* blob = static_cast<const char*>(g_boot_info.dtb_ptr);
    const char* end = blob + g_boot_info.dtb_size;

    for (const char* p = blob; p + KEY_LEN < end; p++) {
        size_t i = 0;
        while (i < KEY_LEN && p[i] == KEY[i]) i++;
        if (i != KEY_LEN) continue;

        const char* q = p + KEY_LEN;
        uint64_t size = 0;
        uint64_t base = 0;
        if (!parse_number(q, end, size) || q >= end || *q != '@') continue;
        q++;
        if (!parse_number(q, end, base)) continue;
        if (size == 0 || base == 0) continue;

        *out_base = base;
        *out_size = size;
        return true;
    }
    return false;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t adopt_boot_framebuffer() {

    if (!display_powered()) {
        log::info("tegra_dc: display controller is off (was a monitor "
                  "attached at power-on?)");
        return ERR_UNAVAILABLE;
    }

    uintptr_t dc_base = 0;
    uintptr_t dc = map_regs(DC_PHYS, DC_SIZE, dc_base);
    if (dc == 0) {
        log::error("tegra_dc: failed to map DC registers at 0x%lx", DC_PHYS);
        return ERR_MAP;
    }

    uint32_t command = dc_read(dc, DC_CMD_DISPLAY_COMMAND);
    if ((command & DISP_CTRL_MODE_MASK) == 0) {
        log::info("tegra_dc: display controller is stopped (command=0x%08x)",
                  command);
        (void)vmm::free(dc_base);
        return ERR_UNAVAILABLE;
    }

    window_state win{};
    bool found = find_active_window(dc, win);
    (void)vmm::free(dc_base);
    if (!found) {
        log::info("tegra_dc: no enabled window is scanning out");
        return ERR_UNAVAILABLE;
    }

    uint8_t red_shift = 0;
    uint8_t blue_shift = 0;
    if (win.depth == WIN_COLOR_DEPTH_B8G8R8A8) {
        red_shift = 16;
        blue_shift = 0;
    } else if (win.depth == WIN_COLOR_DEPTH_R8G8B8A8) {
        // Linux maps this depth to red in the low byte, but the buffer cboot
        // leaves on the P3450 scans out with blue there instead.
        red_shift = 16;
        blue_shift = 0;
    } else {
        log::warn("tegra_dc: unsupported window color depth %u", win.depth);
        return ERR_UNSUPPORTED;
    }

    uint64_t width  = (win.prescaled & 0x7FFF) / 4;
    uint64_t height = (win.prescaled >> 16) & 0x1FFF;
    uint64_t pitch  = win.stride & 0xFFFF;
    uint64_t phys   = static_cast<uint64_t>(win.start)
                    + static_cast<uint64_t>(win.v_offset) * pitch
                    + win.h_offset;

    if (width == 0 || height == 0 || pitch < width * 4) {
        log::warn("tegra_dc: implausible geometry %lux%lu pitch %lu",
                  width, height, pitch);
        return ERR_UNSUPPORTED;
    }


    if (!buffer_is_reserved(phys, pitch * height)) {
        return ERR_UNSAFE;
    }

    auto& fb = g_boot_info.framebuffer;
    fb.fb_phys = phys;
    fb.width = width;
    fb.height = height;
    fb.pitch = pitch;
    fb.bpp = 32;
    fb.red_mask_shift = red_shift;
    fb.green_mask_shift = 8;
    fb.blue_mask_shift = blue_shift;

    log::info("tegra_dc: adopted boot framebuffer %lux%lu pitch %lu at 0x%lx "
              "(display %ux%u)",
              width, height, pitch, phys,
              win.size & 0x1FFF, (win.size >> 16) & 0x1FFF);
    return OK;
}

} // namespace tegra_dc

#endif // STLX_PLATFORM_JETSON_NANO
