#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define GFXFB_GET_INFO 0x4700

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

int main(void) {
    int fd = open("/dev/gfxfb", O_RDWR);
    if (fd < 0) {
        printf("fbtest: open /dev/gfxfb failed\n");
        return 1;
    }

    struct gfxfb_info info;
    if (ioctl(fd, GFXFB_GET_INFO, &info) < 0) {
        printf("fbtest: ioctl GFXFB_GET_INFO failed\n");
        close(fd);
        return 1;
    }

    printf("fbtest: %lux%lu pitch=%lu bpp=%u size=%lu\n",
           info.width, info.height, info.pitch, info.bpp, info.size);

    uint8_t *fb = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        printf("fbtest: mmap failed\n");
        close(fd);
        return 1;
    }

    uint32_t bytes_pp = info.bpp / 8;
    uint32_t rx = 100, ry = 100, rw = 200, rh = 200;

    for (uint32_t y = ry; y < ry + rh && y < info.height; y++) {
        for (uint32_t x = rx; x < rx + rw && x < info.width; x++) {
            uint8_t *pixel = fb + y * info.pitch + x * bytes_pp;
            pixel[info.red_shift / 8]   = 0xFF;
            pixel[info.green_shift / 8] = 0xFF;
            pixel[info.blue_shift / 8]  = 0xFF;
            if (bytes_pp == 4) {
                pixel[3] = 0xFF;
            }
        }
    }

    printf("fbtest: rectangle drawn at (%u,%u) %ux%u\n", rx, ry, rw, rh);

    munmap(fb, info.size);
    close(fd);
    return 0;
}
