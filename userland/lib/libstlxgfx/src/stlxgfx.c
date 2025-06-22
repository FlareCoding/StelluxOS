#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stlxgfx/stlxgfx.h"

// Define STB TrueType implementation before including context header
#define STB_TRUETYPE_IMPLEMENTATION
#include <stlxgfx/internal/stlxgfx_ctx.h>

stlxgfx_context_t* stlxgfx_init(stlxgfx_mode_t mode) {
    stlxgfx_context_t* ctx = malloc(sizeof(stlxgfx_context_t));
    if (!ctx) {
        return NULL;
    }

    memset(ctx, 0, sizeof(stlxgfx_context_t));
    ctx->mode = mode;
    ctx->initialized = 1;

    printf("STLXGFX: Initialized in %s mode\n", 
           mode == STLXGFX_MODE_DISPLAY_MANAGER ? "Display Manager" : "Application");

    return ctx;
}

void stlxgfx_cleanup(stlxgfx_context_t* ctx) {
    if (!ctx) return;

    printf("STLXGFX: Cleaning up context\n");
    
    // Clean up font data
    if (ctx->font_data) {
        free(ctx->font_data);
    }
    
    ctx->initialized = 0;
    free(ctx);
}
