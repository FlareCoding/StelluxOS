#ifndef STLXTERM_OVERLAY_H
#define STLXTERM_OVERLAY_H

#include <stdint.h>
#include <stlxgfx/surface.h>

#define OVERLAY_TEXT_MAX      192
#define OVERLAY_PREVIEW_LINES 8

// A question over the window behind a scrim: a title, a body, optional numbered lines in a recessed box and a
// note, and a cancel button beside the default one
typedef struct {
    int width;
    char title[OVERLAY_TEXT_MAX];
    char body[OVERLAY_TEXT_MAX];
    char preview[OVERLAY_PREVIEW_LINES][OVERLAY_TEXT_MAX];
    int preview_count;
    char preview_more[OVERLAY_TEXT_MAX];
    char note[OVERLAY_TEXT_MAX];
    char cancel[OVERLAY_TEXT_MAX];
    char accept[OVERLAY_TEXT_MAX];
    int is_accept_focused;
} overlay_question_t;

typedef enum {
    OVERLAY_HIT_NONE,
    OVERLAY_HIT_CANCEL,
    OVERLAY_HIT_ACCEPT,
} overlay_hit_t;

// Each draws onto the window's whole surface. Returns 0, or -1 when the faces cannot be opened.
int overlay_draw_copied(stlxgfx_surface_t *s, const char *figure);
int overlay_draw_question(stlxgfx_surface_t *s, const overlay_question_t *question);

// The button of the last question drawn under a window position
overlay_hit_t overlay_question_hit(int32_t x, int32_t y);

#endif // STLXTERM_OVERLAY_H
