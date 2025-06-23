#include <stlxgfx/font.h>
#include <stlxgfx/internal/stlxgfx_ctx.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int stlxgfx_dm_load_font(stlxgfx_context_t* ctx, const char* font_path) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        printf("STLXGFX: Font loading only available in Display Manager mode\n");
        return -1;
    }
    
    if (!font_path) {
        printf("STLXGFX: Invalid font path\n");
        return -1;
    }
    
    // Open font file
    int fd = open(font_path, O_RDONLY);
    if (fd < 0) {
        printf("STLXGFX: Failed to open font file: %s (errno: %d - %s)\n", 
               font_path, errno, strerror(errno));
        return -1;
    }
    
    // Get file size
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        printf("STLXGFX: Failed to get font file size (errno: %d - %s)\n", 
               errno, strerror(errno));
        close(fd);
        return -1;
    }
    
    if (lseek(fd, 0, SEEK_SET) < 0) {
        printf("STLXGFX: Failed to seek to beginning of font file (errno: %d - %s)\n", 
               errno, strerror(errno));
        close(fd);
        return -1;
    }
    
    // Allocate memory for font data
    void* font_data = malloc(file_size);
    if (!font_data) {
        printf("STLXGFX: Failed to allocate %ld bytes for font data\n", file_size);
        close(fd);
        return -1;
    }
    
    // Read font file
    ssize_t total_read = 0;
    char* buffer = (char*)font_data;
    
    while (total_read < file_size) {
        ssize_t bytes_read = read(fd, buffer + total_read, file_size - total_read);
        if (bytes_read < 0) {
            printf("STLXGFX: Failed to read from font file (errno: %d - %s)\n", 
                   errno, strerror(errno));
            free(font_data);
            close(fd);
            return -1;
        }
        if (bytes_read == 0) break;
        total_read += bytes_read;
    }
    
    close(fd);
    
    // Basic TTF validation
    if (total_read >= 4) {
        uint8_t* data = (uint8_t*)font_data;
        if (!((data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00) ||
              (data[0] == 'O' && data[1] == 'T' && data[2] == 'T' && data[3] == 'O'))) {
            printf("STLXGFX: WARNING - Font file may not be a valid TTF/OpenType font\n");
        }
    }
    
    // Initialize STB TrueType
    if (!stbtt_InitFont(&ctx->font_info, font_data, 0)) {
        printf("STLXGFX: Failed to initialize STB TrueType font\n");
        free(font_data);
        return -1;
    }
    
    // Clean up old font data if any
    if (ctx->font_data) {
        free(ctx->font_data);
    }
    
    // Store font data
    ctx->font_data = font_data;
    ctx->font_data_size = total_read;
    ctx->font_loaded = 1;

    return 0;
}

int stlxgfx_dm_get_font_metrics(stlxgfx_context_t* ctx, stlxgfx_font_metrics_t* metrics) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        return -1;
    }
    
    if (!ctx->font_loaded || !metrics) {
        return -1;
    }
    
    stbtt_GetFontVMetrics(&ctx->font_info, &metrics->ascent, &metrics->descent, &metrics->line_gap);
    
    printf("STLXGFX: Font metrics - ascent: %d, descent: %d, line_gap: %d\n",
           metrics->ascent, metrics->descent, metrics->line_gap);
    
    return 0;
}

int stlxgfx_get_text_size(stlxgfx_context_t* ctx, const char* text, 
                             int font_size, stlxgfx_text_size_t* size) {
    if (!ctx || !ctx->initialized || ctx->mode != STLXGFX_MODE_DISPLAY_MANAGER) {
        return -1;
    }
    
    if (!ctx->font_loaded || !text || !size) {
        return -1;
    }
    
    float scale = stbtt_ScaleForPixelHeight(&ctx->font_info, font_size);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&ctx->font_info, &ascent, &descent, &line_gap);
    
    size->height = (int)((ascent - descent) * scale);
    size->width = 0;
    
    // Calculate total width
    for (int i = 0; text[i]; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&ctx->font_info, text[i], &advance, &lsb);
        size->width += (int)(advance * scale);
    }
    
    return 0;
}

