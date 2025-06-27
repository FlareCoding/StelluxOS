#define _POSIX_C_SOURCE 199309L
#include "stlxdm_splash.h"
#include "stlxdm_sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <stlxgfx/internal/stlxgfx_dm.h>
#include <stlxgfx/surface.h>
#include <stlibc/stlibc.h>

// Splash screen configuration
#define SPLASH_FRAME_RATE 60  // Target 60 FPS
#define SPLASH_FRAME_DELAY_MS (1000 / SPLASH_FRAME_RATE)

// Typing animation configuration
#define TYPING_SPEED_MS 14  // <n> ms per character
#define TYPING_DISPLAY_TIME_MS 1200  // <n>> seconds display time
#define TYPING_DELETE_SPEED_MS 10  // <n> ms per character deletion

// Color definitions
#define SPLASH_BG_DARK        0xFF0A0A0A
#define SPLASH_CYAN_BORDER    0xFF00FFFF
#define SPLASH_WHITE_BRIGHT   0xFFFFFFFF
#define SPLASH_WHITE_MIN      0xFF808080

// Logo dimensions
#define LOGO_WIDTH            600
#define LOGO_HEIGHT           80
#define LOGO_BORDER_WIDTH     10
#define LOGO_BORDER_RADIUS    15

// Typing animation states
typedef enum {
    TYPING_STATE_IDLE,
    TYPING_STATE_TYPING,
    TYPING_STATE_DISPLAY,
    TYPING_STATE_DELETING
} typing_state_t;

static void create_dark_background(stlxgfx_surface_t* surface) {
    if (!surface) return;
    stlxgfx_clear_surface(surface, SPLASH_BG_DARK);
}

static void draw_animated_gradient_logo(stlxgfx_surface_t* surface, uint32_t frame) {
    if (!surface) return;
    
    uint32_t center_x = surface->width / 2;
    uint32_t logo_y = surface->height / 3;
    uint32_t logo_x = center_x - LOGO_WIDTH / 2;
    
    // Create flowing gradient animation
    for (uint32_t y = logo_y; y < logo_y + LOGO_HEIGHT; y++) {
        for (uint32_t x = logo_x; x < logo_x + LOGO_WIDTH; x++) {
            float progress_x = (float)(x - logo_x) / LOGO_WIDTH;
            float progress_y = (float)(y - logo_y) / LOGO_HEIGHT;
            
            // Add flowing animation to the gradient
            float flow_offset = sinf((float)frame * 0.02f + progress_x * 3.14159f) * 0.1f;
            progress_x += flow_offset;
            
            if (progress_x < 0.0f) progress_x = 0.0f;
            if (progress_x > 1.0f) progress_x = 1.0f;
            
            // Deep purple to magenta gradient with vertical variation
            uint32_t red = (uint32_t)(40 + progress_x * 140 + progress_y * 15);
            uint32_t green = (uint32_t)(0 + progress_y * 20);
            uint32_t blue = (uint32_t)(80 + progress_x * 120 + progress_y * 10);
            
            if (red > 255) red = 255;
            if (green > 255) green = 255;
            if (blue > 255) blue = 255;
            
            uint32_t color = 0xFF000000 | (red << 16) | (green << 8) | blue;
            stlxgfx_draw_pixel(surface, x, y, color);
        }
    }
}

static void draw_cyan_border(stlxgfx_surface_t* surface) {
    if (!surface) return;
    
    uint32_t center_x = surface->width / 2;
    uint32_t logo_y = surface->height / 3;
    uint32_t logo_x = center_x - LOGO_WIDTH / 2;
    
    stlxgfx_draw_rounded_rect(surface, 
                              logo_x - LOGO_BORDER_WIDTH, 
                              logo_y - LOGO_BORDER_WIDTH,
                              LOGO_WIDTH + (2 * LOGO_BORDER_WIDTH), 
                              LOGO_HEIGHT + (2 * LOGO_BORDER_WIDTH), 
                              LOGO_BORDER_RADIUS, 
                              SPLASH_CYAN_BORDER);
}

static void draw_static_title(stlxgfx_surface_t* surface, stlxgfx_context_t* gfx_ctx) {
    if (!surface || !gfx_ctx) return;
    
    const char* title = "Display Manager v0.1.0";
    uint32_t center_x = surface->width / 2;
    uint32_t logo_y = surface->height / 3;
    uint32_t title_y = logo_y + LOGO_HEIGHT + 30;
    
    uint32_t text_width = strlen(title) * 28 * 0.6f;
    uint32_t title_x = center_x - text_width / 2;
    
    stlxgfx_render_text(gfx_ctx, surface, title, title_x, title_y, 28, SPLASH_WHITE_BRIGHT);
}

static void draw_breathing_text(stlxgfx_surface_t* surface, stlxgfx_context_t* gfx_ctx,
                               uint32_t frame) {
    if (!surface || !gfx_ctx) return;
    
    const char* text = "Press Enter to continue...";
    uint32_t center_x = surface->width / 2;
    uint32_t logo_y = surface->height / 3;
    uint32_t text_y = logo_y + LOGO_HEIGHT + 130;
    
    // Slower breathing effect
    float breath = 0.5f + 0.5f * sinf((float)frame * 0.15f);
    
    // Interpolate between minimum brightness and full brightness
    uint8_t min_intensity = 0x80;
    uint8_t max_intensity = 0xFF;
    uint8_t intensity = (uint8_t)(min_intensity + (max_intensity - min_intensity) * breath);
    
    uint32_t text_color = 0xFF000000 | (intensity << 16) | (intensity << 8) | intensity;
    
    uint32_t text_width = strlen(text) * 16 * 0.6f;
    uint32_t text_x = center_x - text_width / 2;
    
    stlxgfx_render_text(gfx_ctx, surface, text, text_x, text_y, 16, text_color);
}

static void add_modern_decorative_elements(stlxgfx_surface_t* surface, uint32_t frame) {
    if (!surface) return;
    
    uint32_t center_x = surface->width / 2;
    uint32_t logo_y = surface->height / 3;
    uint32_t logo_x = center_x - LOGO_WIDTH / 2;
    
    // Add breathing corner accents
    float breath = 0.5f + 0.5f * sinf((float)frame * 0.15f);
    uint8_t min_intensity = 0x40;
    uint8_t max_intensity = 0xFF;
    uint8_t corner_intensity = (uint8_t)(min_intensity + (max_intensity - min_intensity) * breath);
    uint32_t corner_color = 0xFF000000 | (corner_intensity << 16) | (corner_intensity << 8) | corner_intensity;
    
    uint32_t corner_size = 8;
    
    // Top-left corner
    stlxgfx_fill_rect(surface, logo_x - LOGO_BORDER_WIDTH - corner_size, 
                      logo_y - LOGO_BORDER_WIDTH - corner_size, corner_size, 2, corner_color);
    stlxgfx_fill_rect(surface, logo_x - LOGO_BORDER_WIDTH - corner_size, 
                      logo_y - LOGO_BORDER_WIDTH - corner_size, 2, corner_size, corner_color);
    
    // Top-right corner
    stlxgfx_fill_rect(surface, logo_x + LOGO_WIDTH + LOGO_BORDER_WIDTH, 
                      logo_y - LOGO_BORDER_WIDTH - corner_size, corner_size, 2, corner_color);
    stlxgfx_fill_rect(surface, logo_x + LOGO_WIDTH + LOGO_BORDER_WIDTH + corner_size - 2, 
                      logo_y - LOGO_BORDER_WIDTH - corner_size, 2, corner_size, corner_color);
    
    // Bottom-left corner (rotated 90 degrees clockwise from top-left)
    stlxgfx_fill_rect(surface, logo_x - LOGO_BORDER_WIDTH - corner_size, 
                      logo_y + LOGO_HEIGHT + LOGO_BORDER_WIDTH, 2, corner_size, corner_color);
    stlxgfx_fill_rect(surface, logo_x - LOGO_BORDER_WIDTH - corner_size, 
                      logo_y + LOGO_HEIGHT + LOGO_BORDER_WIDTH + corner_size - 2, corner_size, 2, corner_color);
    
    // Bottom-right corner (rotated 90 degrees clockwise from top-right)
    stlxgfx_fill_rect(surface, logo_x + LOGO_WIDTH + LOGO_BORDER_WIDTH + corner_size - 2, 
                      logo_y + LOGO_HEIGHT + LOGO_BORDER_WIDTH, 2, corner_size, corner_color);
    stlxgfx_fill_rect(surface, logo_x + LOGO_WIDTH + LOGO_BORDER_WIDTH, 
                      logo_y + LOGO_HEIGHT + LOGO_BORDER_WIDTH + corner_size - 2, corner_size, 2, corner_color);
}

static void draw_typing_animation(stlxgfx_surface_t* surface, stlxgfx_context_t* gfx_ctx,
                                 uint32_t frame, uint32_t elapsed_ms) {
    if (!surface || !gfx_ctx) return;
    
    static typing_state_t state = TYPING_STATE_IDLE;
    static uint32_t state_start_time = 0;
    static uint32_t current_length = 0;
    static uint32_t last_animation_time = 0;
    (void) frame;
    
    const char* full_text = "User: root";
    const uint32_t full_length = strlen(full_text);
    
    // Initialize state timing
    if (state == TYPING_STATE_IDLE) {
        state = TYPING_STATE_TYPING;
        state_start_time = elapsed_ms;
        current_length = 0;
        last_animation_time = elapsed_ms;
    }
    
    // Calculate position in the middle of the gradient rectangle
    uint32_t center_x = surface->width / 2;
    uint32_t logo_y = surface->height / 3;
    uint32_t logo_x = center_x - LOGO_WIDTH / 2;
    uint32_t text_x = logo_x + LOGO_WIDTH / 2;
    uint32_t text_y = logo_y + LOGO_HEIGHT / 2 - 4;
    
    // Handle state transitions
    switch (state) {
        case TYPING_STATE_TYPING: {
            // Type out characters
            if (elapsed_ms - last_animation_time >= TYPING_SPEED_MS) {
                if (current_length < full_length) {
                    current_length++;
                    last_animation_time = elapsed_ms;
                } else {
                    // Finished typing, move to display state
                    state = TYPING_STATE_DISPLAY;
                    state_start_time = elapsed_ms;
                }
            }
            break;
        }
        
        case TYPING_STATE_DISPLAY: {
            // Display full text for 2 seconds
            if (elapsed_ms - state_start_time >= TYPING_DISPLAY_TIME_MS) {
                state = TYPING_STATE_DELETING;
                state_start_time = elapsed_ms;
                last_animation_time = elapsed_ms;
            }
            break;
        }
        
        case TYPING_STATE_DELETING: {
            // Delete characters one by one
            if (elapsed_ms - last_animation_time >= TYPING_DELETE_SPEED_MS) {
                if (current_length > 0) {
                    current_length--;
                    last_animation_time = elapsed_ms;
                } else {
                    // Finished deleting, restart typing
                    state = TYPING_STATE_TYPING;
                    state_start_time = elapsed_ms;
                    last_animation_time = elapsed_ms;
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    // Draw the current text
    if (current_length > 0) {
        // Create temporary string with current length
        char temp_text[32];
        strncpy(temp_text, full_text, current_length);
        temp_text[current_length] = '\0';
        
        // Calculate text width for centering
        uint32_t text_width = current_length * 16 * 0.6f; // Approximate width
        uint32_t draw_x = text_x - text_width / 2;
        
        // Draw the typing text in bright green
        stlxgfx_render_text(gfx_ctx, surface, temp_text, draw_x, text_y, 18, SPLASH_WHITE_BRIGHT);
    }
}

static int check_for_enter_key() {
    input_event_t events[16];
    int n = stlx_read_input_events(STLXGFX_INPUT_QUEUE_ID_SYSTEM, 0, events, 16);
    
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (events[i].type == STLXGFX_KBD_EVT_KEY_PRESSED) {
                if (events[i].udata1 == 40) {
                    return 1; // Enter pressed
                }
            }
        }
    }
    
    return 0; // No Enter key
}

int stlxdm_show_splash_screen(stlxdm_compositor_t* compositor) {
    if (!compositor || !compositor->initialized) {
        printf("[STLXDM_SPLASH] ERROR: Invalid compositor for splash screen\n");
        return -1;
    }
    
    printf("[STLXDM_SPLASH] Starting splash screen...\n");
    
    const struct gfx_framebuffer_info* fb_info = stlxdm_compositor_get_fb_info(compositor);
    if (!fb_info) {
        printf("[STLXDM_SPLASH] ERROR: No framebuffer info available\n");
        return -1;
    }
    
    stlxgfx_surface_t* splash_surface = stlxgfx_dm_create_surface(
        compositor->gfx_ctx,
        fb_info->width,
        fb_info->height,
        compositor->gop_format
    );
    
    if (!splash_surface) {
        printf("[STLXDM_SPLASH] ERROR: Failed to create splash surface\n");
        return -1;
    }
    
    uint32_t frame = 0;
    uint32_t elapsed_ms = 0;
    printf("[STLXDM_SPLASH] Displaying splash screen (press Enter to continue)\n");
    
    while (1) {
        create_dark_background(splash_surface);
        draw_animated_gradient_logo(splash_surface, frame);
        draw_typing_animation(splash_surface, compositor->gfx_ctx, frame, elapsed_ms);
        draw_cyan_border(splash_surface);
        draw_static_title(splash_surface, compositor->gfx_ctx);
        draw_breathing_text(splash_surface, compositor->gfx_ctx, frame);
        add_modern_decorative_elements(splash_surface, frame);
        
        stlxdm_begin_frame();
        int present_result = stlxgfx_blit_surface_to_buffer(
            splash_surface, 
            compositor->framebuffer, 
            fb_info->pitch
        );
        stlxdm_end_frame();
        
        if (present_result != 0) {
            printf("[STLXDM_SPLASH] Warning: Failed to present splash frame\n");
        }
        
        if (check_for_enter_key()) {
            printf("[STLXDM_SPLASH] Enter key pressed, continuing to display manager\n");
            break;
        }
        
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = SPLASH_FRAME_DELAY_MS * 1000000
        };
        nanosleep(&delay, NULL);
        
        frame++;
        elapsed_ms += SPLASH_FRAME_DELAY_MS;
    }
    
    // Launch terminal after splash screen exits
    stlxdm_launch_terminal();
    
    stlxgfx_dm_destroy_surface(compositor->gfx_ctx, splash_surface);
    printf("[STLXDM_SPLASH] Splash screen completed\n");
    return 0;
}
