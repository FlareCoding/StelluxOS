#ifndef STLXDM_SPLASH_H
#define STLXDM_SPLASH_H

#include "stlxdm_compositor.h"

/**
 * Display a splash screen with animated elements
 * @param compositor - initialized compositor with framebuffer info
 * @return 0 on success, negative on error
 */
int stlxdm_show_splash_screen(stlxdm_compositor_t* compositor);

#endif // STLXDM_SPLASH_H
