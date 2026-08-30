#include "presenter.hpp"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr unsigned long GFXFB_GET_INFO = 0x4700;

struct gfxfb_info {
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    uint8_t  padding[3];
    uint64_t size;
};

} // namespace

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

    /* The scanout mapping is write-combining, reads from it are
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

    m_back = static_cast<uint8_t*>(
        malloc((size_t)m_width * m_height * 4));
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
        memcpy(m_scanout + (size_t)row * m_pitch + (size_t)r.x * 4,
               m_back + ((size_t)row * m_width + (size_t)r.x) * 4,
               (size_t)r.w * 4);
    }
}

void memcpy_presenter::present(const damage_list& damage) {
    if (damage.full()) {
        damage_list::rect whole = { 0, 0, (int32_t)m_width,
                                    (int32_t)m_height };
        copy_rect(whole);
        return;
    }

    for (uint32_t i = 0; i < damage.count(); i++) {
        damage_list::rect r = damage.at(i);
        if (damage_list::clip(r, (int32_t)m_width, (int32_t)m_height)) {
            copy_rect(r);
        }
    }
}
