#include "screen.hpp"

#include <cstdio>
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

int screen::init() {
    fd = open("/dev/gfxfb", O_RDWR);
    if (fd < 0) {
        return -1;
    }

    gfxfb_info info;
    if (ioctl(fd, GFXFB_GET_INFO, &info) < 0) {
        close(fd);
        fd = -1;
        return -1;
    }

    void* mem = mmap(nullptr, info.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        fd = -1;
        return -1;
    }

    scanout = static_cast<uint8_t*>(mem);
    width = static_cast<uint32_t>(info.width);
    height = static_cast<uint32_t>(info.height);
    pitch = static_cast<uint32_t>(info.pitch);
    bpp = info.bpp;
    red_shift = info.red_shift;
    green_shift = info.green_shift;
    blue_shift = info.blue_shift;
    size = info.size;

    return 0;
}

void screen::shutdown() {
    if (scanout) {
        munmap(scanout, size);
        scanout = nullptr;
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}
