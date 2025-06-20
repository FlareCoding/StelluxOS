#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <stlibc/stlibc.h>

// Define STB TrueType implementation
#define STB_TRUETYPE_IMPLEMENTATION

// Suppress warnings for STB TrueType library
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

#include "stb_truetype.h"

#pragma GCC diagnostic pop

// Declare the syscall function
extern long syscall(uint64_t syscall_number, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Define syscall numbers
#define SYSCALL_SYS_GET_FILE_SIZE_AND_FB_INFO 791
#define SYSCALL_SYS_MAP_FRAMEBUFFER           792
#define SYSCALL_SYS_LOAD_FONT_DATA            793

// Define the info struct that matches the kernel
struct file_size_and_fb_info {
    uint64_t font_file_size;     // Size of UbuntuMono-Regular.ttf
    uint32_t fb_width;           // Framebuffer width
    uint32_t fb_height;          // Framebuffer height  
    uint32_t fb_pitch;           // Framebuffer pitch
    uint8_t  fb_bpp;             // Bits per pixel
};

int main() {
    // Test malloc/free functionality
    int* arr = (int*)malloc(sizeof(int) * 10);
    if (!arr) {
        return -1;
    }
    printf("StelluxOS Init Process: pid %d\n", getpid());
    
    // Test the new syscalls
    printf("Testing font rendering syscalls...\n");
    
    // Test GET_FILE_SIZE_AND_FB_INFO syscall
    struct file_size_and_fb_info info;
    long result = syscall(SYSCALL_SYS_GET_FILE_SIZE_AND_FB_INFO, (uint64_t)&info, 0, 0, 0, 0, 0);
    printf("GET_FILE_SIZE_AND_FB_INFO syscall returned: %ld\n", result);
    
    if (result == 0) {
        printf("Font file size: %lu bytes\n", info.font_file_size);
        printf("Framebuffer: %ux%u, pitch: %u, bpp: %u\n", 
               info.fb_width, info.fb_height, info.fb_pitch, info.fb_bpp);
    }
    
    // Test MAP_FRAMEBUFFER syscall
    uint8_t* mapped_addr = (uint8_t*)syscall(SYSCALL_SYS_MAP_FRAMEBUFFER, 0x1000000, 0, 0, 0, 0, 0);
    printf("MAP_FRAMEBUFFER syscall returned mapped addr: %p\n", mapped_addr);
    
    if (!mapped_addr) {
        printf("Failed to map framebuffer\n");
        return -1;
    }

    uint8_t* framebuffer = (uint8_t*)mapped_addr;
    printf("Framebuffer mapped at: 0x%lx\n", (uintptr_t)framebuffer);
    
    // Test writing to the framebuffer - fill with a red pattern
    printf("Writing test pattern to framebuffer...\n");
    for (int y = 0; y < 100; y++) {
        for (int x = 0; x < 100; x++) {
            int offset = y * info.fb_pitch + x * 3; // 24bpp = 3 bytes per pixel
            framebuffer[offset + 0] = 0xFF; // Blue
            framebuffer[offset + 1] = 0x00; // Green  
            framebuffer[offset + 2] = 0x00; // Red
        }
    }
    printf("Test pattern written to framebuffer!\n");
    
    // Test font loading
    printf("Loading font data...\n");
    uint8_t* font_data = malloc(info.font_file_size);
    if (!font_data) {
        printf("Failed to allocate memory for font data\n");
        return -1;
    }
    
    long bytes_loaded = syscall(SYSCALL_SYS_LOAD_FONT_DATA, (uint64_t)font_data, info.font_file_size, 0, 0, 0, 0);
    printf("LOAD_FONT_DATA syscall returned: %ld bytes\n", bytes_loaded);
    
    if (bytes_loaded > 0) {
        printf("Font data loaded successfully! First few bytes: %02x %02x %02x %02x\n", 
               font_data[0], font_data[1], font_data[2], font_data[3]);
        
        // Verify it's a valid TTF file (should start with 0x00 0x01 0x00 0x00)
        if (font_data[0] == 0x00 && font_data[1] == 0x01 && 
            font_data[2] == 0x00 && font_data[3] == 0x00) {
            printf("Font file appears to be valid TTF!\n");
        } else {
            printf("Warning: Font file doesn't have expected TTF header\n");
        }
        
        // Step 5: Initialize STB TrueType Library and Render Text
        printf("Initializing STB TrueType library...\n");
        
        // Re-allocate font data for the library
        font_data = malloc(info.font_file_size);
        if (!font_data) {
            printf("Failed to allocate memory for font data\n");
            return -1;
        }
        
        // Load font data again
        bytes_loaded = syscall(SYSCALL_SYS_LOAD_FONT_DATA, (uint64_t)font_data, info.font_file_size, 0, 0, 0, 0);
        if (bytes_loaded <= 0) {
            printf("Failed to load font data for rendering\n");
            free(font_data);
            return -1;
        }
        
        // Initialize STB TrueType font
        stbtt_fontinfo font;
        if (!stbtt_InitFont(&font, font_data, 0)) {
            printf("Failed to initialize STB TrueType font\n");
            free(font_data);
            return -1;
        }
        
        printf("STB TrueType font initialized successfully!\n");
        
        // Set up rendering parameters
        float scale = stbtt_ScaleForPixelHeight(&font, 32.0f); // 24 pixel height
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
        
        printf("Font metrics - Ascent: %d, Descent: %d, LineGap: %d, Scale: %.2f\n", 
               ascent, descent, lineGap, scale);
        
        // Render some text
        const char* text = "Hello StelluxOS!";
        int x = 180, y = 140;
        
        printf("Rendering text: '%s' at position (%d, %d)\n", text, x, y);
        
        for (int i = 0; text[i]; i++) {
            int codepoint = text[i];
            
            // Get character metrics
            int advance, lsb;
            stbtt_GetCodepointHMetrics(&font, codepoint, &advance, &lsb);
            
            // Get character bitmap
            int width, height, xoff, yoff;
            unsigned char* bitmap = stbtt_GetCodepointBitmap(&font, scale, scale, codepoint, &width, &height, &xoff, &yoff);
            
            if (bitmap && width > 0 && height > 0) {
                printf("Character '%c': %dx%d at offset (%d, %d), advance: %d\n", 
                       codepoint, width, height, xoff, yoff, advance);
                
                // Draw the character to framebuffer
                int char_x = x + (int)(lsb * scale) + xoff;
                int char_y = y + (int)(ascent * scale) + yoff;
                
                for (int py = 0; py < height; py++) {
                    for (int px = 0; px < width; px++) {
                        int fb_x = char_x + px;
                        int fb_y = char_y + py;
                        
                        // Check bounds
                        if (fb_x >= 0 && fb_x < (int)info.fb_width && 
                            fb_y >= 0 && fb_y < (int)info.fb_height) {
                            
                            // Get alpha value from bitmap
                            unsigned char alpha = bitmap[py * width + px];
                            
                            if (alpha > 0) {
                                // Calculate framebuffer offset
                                int fb_offset = fb_y * info.fb_pitch + fb_x * 3;
                                
                                // Set pixel color (white text)
                                framebuffer[fb_offset + 0] = alpha; // Blue
                                framebuffer[fb_offset + 1] = alpha; // Green
                                framebuffer[fb_offset + 2] = alpha; // Red
                            }
                        }
                    }
                }
                
                // Free the bitmap
                stbtt_FreeBitmap(bitmap, NULL);
            }
            
            // Advance to next character
            x += (int)(advance * scale);
        }
        
        printf("Text rendering completed!\n");
        
        // Clean up
        free(font_data);

        printf("Font rendering syscall tests completed!\n");
    } else {
        printf("Failed to load font data\n");
        free(font_data);
        return -1;
    }

    return 0;
}
