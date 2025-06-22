#ifndef STLXGFX_CTX_H
#define STLXGFX_CTX_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <stlxgfx/internal/stb_truetype.h>
#pragma GCC diagnostic pop

#include <stlxgfx/stlxgfx.h>

#define STLXGFX_PAGE_SIZE 0x1000

// Internal context structure definition
struct stlxgfx_context {
    stlxgfx_mode_t mode;
    int initialized;
    
    // Font management (DM mode only)
    void* font_data;
    size_t font_data_size;
    stbtt_fontinfo font_info;
    int font_loaded;
};

#endif // STLXGFX_CTX_H