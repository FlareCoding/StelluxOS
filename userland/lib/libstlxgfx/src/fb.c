#include <stlxgfx/fb.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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

int stlxgfx_fb_open(stlxgfx_fb_t* fb) {
    if (!fb) {
        return -1;
    }
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    int fd = open("/dev/gfxfb", O_RDWR);
    if (fd < 0) {
        printf("stlxgfx: failed to open /dev/gfxfb\n");
        return -1;
    }

    struct gfxfb_info info;
    if (ioctl(fd, GFXFB_GET_INFO, &info) < 0) {
        printf("stlxgfx: ioctl GFXFB_GET_INFO failed\n");
        close(fd);
        return -1;
    }

    uint8_t* buffer = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer == MAP_FAILED) {
        printf("stlxgfx: mmap framebuffer failed\n");
        close(fd);
        return -1;
    }

    fb->fd          = fd;
    fb->buffer      = buffer;
    fb->width       = (uint32_t)info.width;
    fb->height      = (uint32_t)info.height;
    fb->pitch       = (uint32_t)info.pitch;
    fb->bpp         = info.bpp;
    fb->red_shift   = info.red_shift;
    fb->green_shift = info.green_shift;
    fb->blue_shift  = info.blue_shift;
    fb->size        = info.size;

    return 0;
}

void stlxgfx_fb_close(stlxgfx_fb_t* fb) {
    if (!fb) {
        return;
    }
    if (fb->buffer) {
        munmap(fb->buffer, fb->size);
        fb->buffer = NULL;
    }
    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }
}

stlxgfx_surface_t* stlxgfx_fb_surface(stlxgfx_fb_t* fb) {
    if (!fb || !fb->buffer) {
        return NULL;
    }
    return stlxgfx_surface_from_buffer(fb->buffer,
                                        fb->width, fb->height, fb->pitch,
                                        fb->bpp, fb->red_shift,
                                        fb->green_shift, fb->blue_shift);
}

stlxgfx_surface_t* stlxgfx_fb_create_surface(const stlxgfx_fb_t* fb,
                                               uint32_t width, uint32_t height) {
    if (!fb) {
        return NULL;
    }
    return stlxgfx_create_surface(width, height, fb->bpp,
                                   fb->red_shift, fb->green_shift, fb->blue_shift);
}

void stlxgfx_fb_present(stlxgfx_fb_t* fb, const stlxgfx_surface_t* surface) {
    if (!fb || !fb->buffer || !surface || !surface->pixels) {
        return;
    }

    uint32_t copy_h = surface->height < fb->height ? surface->height : fb->height;
    uint32_t copy_w_bytes = surface->pitch < fb->pitch ? surface->pitch : fb->pitch;

    for (uint32_t y = 0; y < copy_h; y++) {
        const uint8_t* src = surface->pixels + y * surface->pitch;
        uint8_t* dst = fb->buffer + y * fb->pitch;
        memcpy(dst, src, copy_w_bytes);
    }
}
