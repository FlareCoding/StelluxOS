#ifndef STLXGFX_FONT_H
#define STLXGFX_FONT_H

#include <stddef.h>
#include <stdint.h>

typedef struct stlxgfx_context stlxgfx_context_t;

typedef struct {
    int ascent;
    int descent; 
    int line_gap;
} stlxgfx_font_metrics_t;

typedef struct {
    int width;
    int height;
} stlxgfx_text_size_t;

/**
 * Load a font file (Display Manager only)
 * @param ctx - graphics context
 * @param font_path - path to TTF font file
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_load_font(stlxgfx_context_t* ctx, const char* font_path);

/**
 * Get font vertical metrics (Display Manager)
 * @param ctx - graphics context
 * @param metrics - output font metrics
 * @return 0 on success, negative on error  
 */
int stlxgfx_dm_get_font_metrics(stlxgfx_context_t* ctx, stlxgfx_font_metrics_t* metrics);

/**
 * Calculate text size for given string and font size
 * @param ctx - graphics context
 * @param text - null-terminated string
 * @param font_size - font size in pixels
 * @param size - output text dimensions
 * @return 0 on success, negative on error
 */
int stlxgfx_get_text_size(stlxgfx_context_t* ctx, const char* text, 
                            int font_size, stlxgfx_text_size_t* size);

#endif // STLXGFX_FONT_H 