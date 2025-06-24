#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stlxgfx/stlxgfx.h"

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

    printf("STLXGFX: Initializing in %s mode\n", 
           mode == STLXGFX_MODE_DISPLAY_MANAGER ? "Display Manager" : "Application");

    // Initialize socket communication channel
    if (stlxgfx_init_comm_channel(ctx) != 0) {
        printf("STLXGFX: Failed to initialize communication\n");
        free(ctx);
        return NULL;
    }

    ctx->initialized = 1;
    printf("STLXGFX: Initialization complete\n");

    return ctx;
}

void stlxgfx_cleanup(stlxgfx_context_t* ctx) {
    if (!ctx) return;

    printf("STLXGFX: Cleaning up context\n");
    
    // Clean up socket communication channel
    stlxgfx_cleanup_comm_channel(ctx);
    
    // Clean up font data
    if (ctx->font_data) {
        free(ctx->font_data);
    }
    
    ctx->initialized = 0;
    free(ctx);
}
