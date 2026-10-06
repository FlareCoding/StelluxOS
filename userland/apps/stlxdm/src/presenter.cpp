#include "presenter.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

constexpr unsigned long GFXFB_GET_INFO = 0x4700;
constexpr unsigned long GFXFB_FLUSH = 0x4701;

constexpr uint8_t GFXFB_INFO_NEEDS_FLUSH = 0x01;

namespace {

struct gfxfb_info {
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    uint8_t  flags;
    uint8_t  padding[2];
    uint64_t size;
};

struct gfxfb_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct gfxfb_flush_args {
    uint64_t rects;
    uint32_t count;
    uint32_t reserved;
};

} // namespace

static gfxfb_rect to_gfxfb_rect(const damage_list::rect& r) {
    return { static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y),
             static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h) };
}

int memcpy_presenter::init() {
    m_fd = open("/dev/gfxfb", O_RDWR);
    if (m_fd < 0) {
        return -1;
    }

    gfxfb_info info;
    if (ioctl(m_fd, GFXFB_GET_INFO, &info) < 0) {
        close(m_fd);
        m_fd = -1;
        return -1;
    }

    /* The scanout mapping may be write-combining, where reads are
     * pathologically slow, so composition targets m_back instead */
    void* mem = mmap(nullptr, info.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, m_fd, 0);
    if (mem == MAP_FAILED) {
        close(m_fd);
        m_fd = -1;
        return -1;
    }

    m_width = static_cast<uint32_t>(info.width);
    m_height = static_cast<uint32_t>(info.height);
    m_pitch = static_cast<uint32_t>(info.pitch);
    m_size = info.size;
    m_scanout = static_cast<uint8_t*>(mem);
    m_needs_flush = (info.flags & GFXFB_INFO_NEEDS_FLUSH) != 0;

    m_back = static_cast<uint8_t*>(
        malloc(static_cast<size_t>(m_width) * m_height * 4));
    if (!m_back) {
        shutdown();
        return -1;
    }

    return 0;
}

void memcpy_presenter::shutdown() {
    free(m_back);
    m_back = nullptr;

    if (m_scanout) {
        munmap(m_scanout, m_size);
        m_scanout = nullptr;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

presenter::target memcpy_presenter::acquire() {
    target t;
    t.pixels = reinterpret_cast<uint32_t*>(m_back);
    t.stride = m_width * 4;

    /* One persistent buffer: undefined on first use, then it always
     * holds the previous frame */
    t.age = m_first_acquire ? 0 : 1;
    m_first_acquire = false;

    return t;
}

void memcpy_presenter::copy_rect(const damage_list::rect& r) {
    for (int32_t row = r.y; row < r.y + r.h; row++) {
        memcpy(m_scanout + static_cast<size_t>(row) * m_pitch + static_cast<size_t>(r.x) * 4,
               m_back + (static_cast<size_t>(row) * m_width + static_cast<size_t>(r.x)) * 4,
               static_cast<size_t>(r.w) * 4);
    }
}

void memcpy_presenter::flush_display(const damage_list::rect* rects, uint32_t count) {
    if (!m_needs_flush || count == 0) {
        return;
    }

    gfxfb_rect changed[damage_list::MAX_RECTS];
    uint32_t changed_count = 0;

    /* A failed flush left its regions unshown, so the next one sends the whole screen */
    if (m_last_flush_failed) {
        changed[changed_count++] = { 0, 0, m_width, m_height };
    } else {
        for (uint32_t i = 0; i < count; i++) {
            changed[changed_count++] = to_gfxfb_rect(rects[i]);
        }
    }

    gfxfb_flush_args args = {};
    args.rects = reinterpret_cast<uint64_t>(changed);
    args.count = changed_count;

    bool failed = ioctl(m_fd, GFXFB_FLUSH, &args) < 0;
    if (failed && !m_last_flush_failed) {
        printf("stlxdm: display flush failed: %s\r\n", strerror(errno));
    }

    m_last_flush_failed = failed;
}

void memcpy_presenter::present(const damage_list& damage) {
    if (damage.full()) {
        damage_list::rect whole = { 0, 0, static_cast<int32_t>(m_width),
                                    static_cast<int32_t>(m_height) };
        copy_rect(whole);
        flush_display(&whole, 1);
        return;
    }

    damage_list::rect copied[damage_list::MAX_RECTS];
    uint32_t count = 0;

    for (uint32_t i = 0; i < damage.count(); i++) {
        damage_list::rect r = damage.at(i);
        if (damage_list::clip(r, static_cast<int32_t>(m_width), static_cast<int32_t>(m_height))) {
            copy_rect(r);
            copied[count++] = r;
        }
    }

    flush_display(copied, count);
}
