#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "stlxdm.h"

// Define STB TrueType implementation
#define STB_TRUETYPE_IMPLEMENTATION

// Suppress warnings for STB TrueType library
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

#include "stb_truetype.h"

#pragma GCC diagnostic pop

int main() {
    printf("StelluxOS Display Manager (STLXDM) - Initializing...\n");
    
    const char* font_path = "/initrd/res/fonts/UbuntuMono-Regular.ttf";
    
    // Load TTF font file
    int fd = open(font_path, O_RDONLY);
    if (fd < 0) {
        printf("ERROR: Failed to open font file: %s (errno: %d - %s)\n", 
               font_path, errno, strerror(errno));
        return 1;
    }
    
    // Get file size
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        printf("ERROR: Failed to get font file size (errno: %d - %s)\n", 
               errno, strerror(errno));
        close(fd);
        return 1;
    }
    
    if (lseek(fd, 0, SEEK_SET) < 0) {
        printf("ERROR: Failed to seek to beginning of font file (errno: %d - %s)\n", 
               errno, strerror(errno));
        close(fd);
        return 1;
    }
    
    // Allocate memory for font data
    void* font_data = malloc(file_size);
    if (!font_data) {
        printf("ERROR: Failed to allocate %ld bytes for font data\n", file_size);
        close(fd);
        return 1;
    }
    
    // Read font file
    ssize_t total_read = 0;
    char* buffer = (char*)font_data;
    
    while (total_read < file_size) {
        ssize_t bytes_read = read(fd, buffer + total_read, file_size - total_read);
        if (bytes_read < 0) {
            printf("ERROR: Failed to read from font file (errno: %d - %s)\n", 
                   errno, strerror(errno));
            free(font_data);
            close(fd);
            return 1;
        }
        if (bytes_read == 0) break;
        total_read += bytes_read;
    }
    
    // Basic TTF validation
    if (total_read >= 4) {
        uint8_t* data = (uint8_t*)font_data;
        if (!((data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00) ||
              (data[0] == 'O' && data[1] == 'T' && data[2] == 'T' && data[3] == 'O'))) {
            printf("WARNING: Font file may not be a valid TTF/OpenType font\n");
        }
    }
    
    close(fd);

    // Initialize graphics system
    // Get framebuffer info
    struct gfx_framebuffer_info fb_info;
    long result = syscall2(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_GET_INFO, (uint64_t)&fb_info);
    if (result != 0) {
        printf("ERROR: Failed to get framebuffer info: %ld\n", result);
        free(font_data);
        return 1;
    }
    
    printf("Framebuffer: %ux%u, %u BPP\n", fb_info.width, fb_info.height, fb_info.bpp);
    
    // Map framebuffer
    long map_result = syscall1(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_MAP_FRAMEBUFFER);
    if (map_result <= 0) {
        printf("ERROR: Failed to map framebuffer: %ld\n", map_result);
        free(font_data);
        return 1;
    }
    
    // Set up rendering
    uint8_t* framebuffer = (uint8_t*)map_result;
    uint32_t width = fb_info.width;
    uint32_t height = fb_info.height;
    uint32_t bytes_per_pixel = fb_info.bpp / 8;
    uint32_t pitch = fb_info.pitch;
    
    // Helper macro for pixel writing
    #define write_pixel(x, y, color) do { \
        if ((x) < width && (y) < height) { \
            uint8_t* pixel = framebuffer + ((y) * pitch) + ((x) * bytes_per_pixel); \
            uint8_t r = ((color) >> 16) & 0xFF; \
            uint8_t g = ((color) >> 8) & 0xFF; \
            uint8_t b = (color) & 0xFF; \
            if (bytes_per_pixel == 3) { \
                pixel[0] = b; pixel[1] = g; pixel[2] = r; \
            } else if (bytes_per_pixel == 4) { \
                pixel[0] = b; pixel[1] = g; pixel[2] = r; pixel[3] = 0; \
            } \
        } \
    } while(0)
    
    // Clear screen
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            write_pixel(x, y, 0x202020);
        }
    }
    
    // Draw colored squares in corners
    for (uint32_t y = 50; y < 150 && y < height; y++) {
        for (uint32_t x = 50; x < 150 && x < width; x++) {
            write_pixel(x, y, 0xFF0000);  // Red - top-left
        }
    }
    
    for (uint32_t y = 50; y < 150 && y < height; y++) {
        for (uint32_t x = width - 150; x < width - 50 && x < width; x++) {
            write_pixel(x, y, 0x00FF00);  // Green - top-right
        }
    }
    
    for (uint32_t y = height - 150; y < height - 50 && y < height; y++) {
        for (uint32_t x = 50; x < 150 && x < width; x++) {
            write_pixel(x, y, 0x0000FF);  // Blue - bottom-left
        }
    }
    
    for (uint32_t y = height - 150; y < height - 50 && y < height; y++) {
        for (uint32_t x = width - 150; x < width - 50 && x < width; x++) {
            write_pixel(x, y, 0xFFFFFF);  // White - bottom-right
        }
    }
    
    // Draw RGB gradient in center
    uint32_t gradient_start_y = height / 2 - 50;
    uint32_t gradient_end_y = height / 2 + 50;
    for (uint32_t y = gradient_start_y; y < gradient_end_y && y < height; y++) {
        for (uint32_t x = 200; x < width - 200 && x < width; x++) {
            uint32_t red = (255 * (width - 200 - x)) / (width - 400);
            uint32_t blue = (255 * x) / (width - 400);
            uint32_t color = (red << 16) | blue;
            write_pixel(x, y, color);
        }
    }
    
    // Draw checkerboard pattern
    for (uint32_t y = height / 2 - 100; y < height / 2 + 100 && y < height; y++) {
        for (uint32_t x = 50; x < 250 && x < width; x++) {
            int checker = ((x / 20) + (y / 20)) % 2;
            uint32_t color = checker ? 0x808080 : 0xC0C0C0;
            write_pixel(x, y, color);
        }
    }
    
    // Draw diagonal stripes
    for (uint32_t y = height / 2 - 100; y < height / 2 + 100 && y < height; y++) {
        for (uint32_t x = width - 250; x < width - 50 && x < width; x++) {
            int stripe = ((x - (width - 250)) + y) / 15 % 2;
            uint32_t color = stripe ? 0xFF00FF : 0x00FFFF;
            write_pixel(x, y, color);
        }
    }
    
    // Initialize TTF font rendering
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, font_data, 0)) {
        printf("ERROR: Failed to initialize STB TrueType font\n");
        free(font_data);
        return 1;
    }
    
    // Set up font parameters
    float scale = stbtt_ScaleForPixelHeight(&font, 32.0f);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
    
    // Render text
    const char* text = "Hello from StelluxOS!";
    int text_x = 180, text_y = height / 2 + 140;
    
    for (int i = 0; text[i]; i++) {
        int codepoint = text[i];
        
        // Get character metrics
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font, codepoint, &advance, &lsb);
        
        // Get character bitmap
        int char_width, char_height, xoff, yoff;
        unsigned char* bitmap = stbtt_GetCodepointBitmap(&font, scale, scale, codepoint, 
                                                       &char_width, &char_height, &xoff, &yoff);
        
        if (bitmap && char_width > 0 && char_height > 0) {
            // Draw character to framebuffer
            int char_x = text_x + (int)(lsb * scale) + xoff;
            int char_y = text_y + (int)(ascent * scale) + yoff;
            
            for (int py = 0; py < char_height; py++) {
                for (int px = 0; px < char_width; px++) {
                    int fb_x = char_x + px;
                    int fb_y = char_y + py;
                    
                    if (fb_x >= 0 && fb_x < (int)width && 
                        fb_y >= 0 && fb_y < (int)height) {
                        
                        unsigned char alpha = bitmap[py * char_width + px];
                        if (alpha > 0) {
                            uint32_t color = ((uint32_t)alpha << 16) | ((uint32_t)alpha << 8) | (uint32_t)alpha;
                            write_pixel((uint32_t)fb_x, (uint32_t)fb_y, color);
                        }
                    }
                }
            }
            
            stbtt_FreeBitmap(bitmap, NULL);
        }
        
        // Advance to next character
        text_x += (int)(advance * scale);
    }
    
    #undef write_pixel
    
    printf("Graphics rendering complete!\n");
    printf("STLXDM initialization finished - display should be visible\n");

    // Clean up
    free(font_data);
    return 0;
}
