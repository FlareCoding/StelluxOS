#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stlxgfx/stlxgfx.h"
#include "stlxgfx/font.h"

// Define STB TrueType implementation before including context header
#define STB_TRUETYPE_IMPLEMENTATION
#include <stlxgfx/internal/stlxgfx_ctx.h>
#include <stlxgfx/internal/stlxgfx_comm.h>

stlxgfx_context_t* stlxgfx_init(stlxgfx_mode_t mode) {
    stlxgfx_context_t* ctx = malloc(sizeof(stlxgfx_context_t));
    if (!ctx) {
        return NULL;
    }

    memset(ctx, 0, sizeof(stlxgfx_context_t));
    ctx->mode = mode;

    // Initialize socket communication channel
    if (stlxgfx_init_comm_channel(ctx) != 0) {
        printf("STLXGFX: Failed to initialize communication\n");
        free(ctx);
        return NULL;
    }

    // Initialize character cache
    for (int i = 0; i < STLXGFX_CHAR_CACHE_SIZE; i++) {
        ctx->char_cache[i].bitmap = NULL;
        ctx->char_cache[i].valid = 0;
    }
    ctx->cached_font_size = 0;

    // Load default system font for both DM and applications
    const char* font_path = "/initrd/res/fonts/UbuntuMono-Regular.ttf";
    if (stlxgfx_load_font(ctx, font_path) != 0) {
        printf("STLXGFX: Warning - Failed to load system font (%s)\n", font_path);
        printf("STLXGFX: Text rendering will be unavailable\n");
        // Continue initialization even if font loading fails
    }

    ctx->initialized = 1;

    return ctx;
}

void stlxgfx_cleanup(stlxgfx_context_t* ctx) {
    if (!ctx) return;

    // Clean up socket communication channel
    stlxgfx_cleanup_comm_channel(ctx);
    
    // Clean up character cache
    for (int i = 0; i < STLXGFX_CHAR_CACHE_SIZE; i++) {
        if (ctx->char_cache[i].bitmap) {
            free(ctx->char_cache[i].bitmap);
            ctx->char_cache[i].bitmap = NULL;
        }
        ctx->char_cache[i].valid = 0;
    }
    
    // Clean up font data
    if (ctx->font_data) {
        free(ctx->font_data);
    }
    
    ctx->initialized = 0;
    free(ctx);
}
