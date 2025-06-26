#ifndef STLXGFX_H
#define STLXGFX_H

#include <stddef.h>

// =========================
// Library Version
// =========================
#define STLXGFX_VERSION_MAJOR 0
#define STLXGFX_VERSION_MINOR 1
#define STLXGFX_VERSION_PATCH 0

// =========================
// Core Types
// =========================

typedef enum {
    STLXGFX_MODE_APPLICATION,
    STLXGFX_MODE_DISPLAY_MANAGER
} stlxgfx_mode_t;

typedef struct stlxgfx_context stlxgfx_context_t;

// =========================
// Library Initialization
// =========================

/**
 * Initialize the graphics library
 * @param mode - operation mode (application or display manager)
 * @return context pointer or NULL on failure
 */
stlxgfx_context_t* stlxgfx_init(stlxgfx_mode_t mode);

/**
 * Clean up and free library resources
 * @param ctx - context to cleanup
 */
void stlxgfx_cleanup(stlxgfx_context_t* ctx);

// =========================
// Component Headers
// =========================
#include "stlxgfx/font.h"
#include "stlxgfx/surface.h"
#include "stlxgfx/window.h"
#include "stlxgfx/event.h"

#endif // STLXGFX_H 