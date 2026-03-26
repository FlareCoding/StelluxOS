#include <stlxgfx/bmp.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint16_t read_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t read_i32(const uint8_t* p) {
    uint32_t v = read_u32(p);
    return (int32_t)v;
}

stlxgfx_surface_t* stlxgfx_load_bmp(const char* path) {
    if (!path) return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 54) {
        close(fd);
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;
    uint8_t* data = malloc(file_size);
    if (!data) {
        close(fd);
        return NULL;
    }

    size_t total = 0;
    while (total < file_size) {
        ssize_t n = read(fd, data + total, file_size - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);

    if (total != file_size) {
        free(data);
        return NULL;
    }

    if (data[0] != 'B' || data[1] != 'M') {
        free(data);
        return NULL;
    }

    uint32_t pixel_offset = read_u32(data + 10);
    uint32_t dib_size     = read_u32(data + 14);

    if (dib_size != 40 && dib_size != 108 && dib_size != 124) {
        free(data);
        return NULL;
    }

    int32_t  width      = read_i32(data + 18);
    int32_t  height     = read_i32(data + 22);
    uint16_t bpp        = read_u16(data + 28);
    uint32_t compression = read_u32(data + 30);

    if (width <= 0 || width > 4096) {
        free(data);
        return NULL;
    }

    int bottom_up = (height > 0);
    uint32_t abs_h = bottom_up ? (uint32_t)height : (uint32_t)(-height);
    if (abs_h == 0 || abs_h > 4096) {
        free(data);
        return NULL;
    }

    if ((bpp != 24 && bpp != 32) || (compression != 0 && compression != 3)) {
        free(data);
        return NULL;
    }

    uint32_t src_stride = (((uint32_t)width * bpp + 31) / 32) * 4;
    uint32_t pixel_data_size = src_stride * abs_h;

    if (pixel_offset + pixel_data_size > file_size) {
        free(data);
        return NULL;
    }

    /*
     * Output: 32-bit surface with byte order B,G,R,A
     * (blue_shift=0, green_shift=8, red_shift=16, alpha at byte 3)
     */
    stlxgfx_surface_t* surface = stlxgfx_create_surface(
        (uint32_t)width, abs_h, 32, 16, 8, 0);
    if (!surface) {
        free(data);
        return NULL;
    }

    uint8_t* pixels = data + pixel_offset;
    uint32_t dst_stride = surface->pitch;
    uint32_t src_bpp_bytes = bpp / 8;

    for (uint32_t y = 0; y < abs_h; y++) {
        uint32_t src_row = bottom_up ? (abs_h - 1 - y) : y;
        const uint8_t* src = pixels + src_row * src_stride;
        uint8_t* dst = surface->pixels + y * dst_stride;

        for (uint32_t x = 0; x < (uint32_t)width; x++) {
            const uint8_t* sp = src + x * src_bpp_bytes;
            uint8_t* dp = dst + x * 4;
            dp[0] = sp[0]; /* B */
            dp[1] = sp[1]; /* G */
            dp[2] = sp[2]; /* R */
            dp[3] = (bpp == 32) ? sp[3] : 0xFF;
        }
    }

    free(data);
    return surface;
}
