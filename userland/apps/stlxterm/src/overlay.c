#include "overlay.h"
#include <stlxgfx/ctx.h>
#include <stlxgfx/font.h>
#include <stlxtheme/theme.h>
#include <string.h>

/* Lengths of the terminal design's pills and of Sheen's sheets */
#define PILL_HEIGHT       32
#define PILL_PAD_LEFT     12
#define PILL_PAD_RIGHT    10
#define PILL_GAP          8
#define PILL_BOTTOM       16
#define GLYPH_SIZE        14
#define SHEET_MARGIN      16
#define SHEET_PAD_X       24
#define SHEET_PAD_TOP     20
#define SHEET_PAD_BOTTOM  16
#define TITLE_SIZE        15.5f
#define TITLE_LINE        22
#define BODY_GAP          6
#define BODY_LINE         20
#define BODY_LINES_MAX    4
#define PREVIEW_GAP       14
#define PREVIEW_PAD_X     14
#define PREVIEW_PAD_Y     10
#define PREVIEW_LINE      22
#define NUMBER_WIDTH      10
#define NUMBER_GAP        14
#define FOOTER_PAD_X      16
#define FOOTER_PAD_TOP    4
#define FOOTER_PAD_BOTTOM 16
#define NOTE_INSET        8
#define BUTTON_HEIGHT     32
#define BUTTON_PAD_X      14
#define BUTTON_GAP        8
#define SHADOW_OFFSET     8
#define SHADOW_BLUR       20

/* Translucent colors the design gives as values rather than tokens */
#define SCRIM             0x75060402 /* ink at .46 */
#define SHADOW_CORE       0x3A060402 /* ink at .45, halved where a 20 px blur meets the shadow's edge */
#define PILL_RING         0x99060402 /* ink at .60 */
#define SHEET_RING        0xB8060402 /* ink at .72 */
#define SHEET_TOP_LIGHT   0x12FFF0D7 /* cream at .07 */
#define BUTTON_DEFAULT    0x14FFF0D7 /* cream at .08 */
#define BUTTON_FOCUS_RING 0x38FFF0D7 /* cream at .22 */

typedef struct {
    int32_t x, y;
    uint32_t w, h;
} overlay_rect_t;

static stlxgfx_font *g_body;
static stlxgfx_font *g_medium;
static stlxgfx_font *g_title;
static stlxgfx_font *g_caption;
static stlxgfx_font *g_mono;

/* The buttons of the last question drawn, for overlay_question_hit */
static overlay_rect_t g_cancel_rect;
static overlay_rect_t g_accept_rect;

static stlxgfx_font *open_face(stlxgfx_font *face, const char *path, float size) {
    return face ? face : stlxgfx_font_open(path, size);
}

/* The faces open on first use, so a terminal that never shows an overlay never reads them */
static int open_faces(void) {
    g_body = open_face(g_body, STLXTHEME_UI_FONT_PATH, STLXTHEME_BODY_SIZE);
    g_medium = open_face(g_medium, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE);
    g_title = open_face(g_title, STLXTHEME_UI_FONT_SEMIBOLD_PATH, TITLE_SIZE);
    g_caption = open_face(g_caption, STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE);
    g_mono = open_face(g_mono, STLXTHEME_MONO_FONT_PATH, STLXTHEME_BODY_SIZE);
    return g_body && g_medium && g_title && g_caption && g_mono ? 0 : -1;
}

/* The baseline that centers a face's ascent and descent in a line box from top */
static int32_t baseline_in(const stlxgfx_font *face, int32_t top, int32_t line) {
    stlxgfx_font_metrics metrics;
    stlxgfx_font_metrics_get(face, &metrics);
    return top + (line - (metrics.ascent + metrics.descent)) / 2 + metrics.ascent;
}

static void draw_line_text(stlxgfx_surface_t *s, const stlxgfx_font *face, int32_t x, int32_t top, int32_t line,
                           const char *text, uint32_t color) {
    stlxgfx_draw_text(s, face, x, baseline_in(face, top, line), text, strlen(text), color);
}

/* Draws text cut to max_width, an ellipsis standing for what does not fit */
static void draw_fitted_text(stlxgfx_surface_t *s, const stlxgfx_font *face, int32_t x, int32_t top, int32_t line,
                             const char *text, int32_t max_width, uint32_t color) {
    static const char ellipsis[] = "\xe2\x80\xa6";
    size_t len = strlen(text);
    int32_t baseline = baseline_in(face, top, line);
    if (stlxgfx_text_width(face, text, len) <= max_width) {
        stlxgfx_draw_text(s, face, x, baseline, text, len, color);
        return;
    }

    int32_t room = max_width - stlxgfx_text_width(face, ellipsis, sizeof(ellipsis) - 1);
    while (len > 0 && ((text[len] & 0xC0) == 0x80 || stlxgfx_text_width(face, text, len) > room)) {
        len--;
    }

    stlxgfx_draw_text(s, face, x, baseline, text, len, color);
    stlxgfx_draw_text(s, face, x + stlxgfx_text_width(face, text, len), baseline, ellipsis, sizeof(ellipsis) - 1,
                      color);
}

/* A soft shadow under a rounded rect, offset down: a core under the rect and a halo fading out around it */
static void draw_shadow(stlxgfx_ctx_t *ctx, overlay_rect_t r, uint32_t radius) {
    int32_t y = r.y + SHADOW_OFFSET;
    stlxgfx_ctx_fill_rounded_rect(ctx, r.x, y, r.w, r.h, radius, SHADOW_CORE);

    uint32_t core_alpha = SHADOW_CORE >> 24;
    for (int32_t d = 1; d < SHADOW_BLUR; d++) {
        uint32_t fade = (uint32_t)(SHADOW_BLUR - d);
        uint32_t alpha = core_alpha * fade * fade / (SHADOW_BLUR * SHADOW_BLUR);
        uint32_t color = alpha << 24 | (SHADOW_CORE & 0x00FFFFFF);
        stlxgfx_ctx_draw_rounded_rect(ctx, r.x - d, y - d, r.w + 2 * (uint32_t)d, r.h + 2 * (uint32_t)d,
                                      radius + (uint32_t)d, color);
    }
}

/* A floating panel of the Razor look: shadow, an ink ring, the menu fill and the rim */
static void draw_panel(stlxgfx_ctx_t *ctx, overlay_rect_t r, uint32_t radius, uint32_t ring) {
    draw_shadow(ctx, r, radius);
    stlxgfx_ctx_draw_rounded_rect(ctx, r.x - 1, r.y - 1, r.w + 2, r.h + 2, radius + 1, ring);
    stlxgfx_ctx_fill_rounded_rect(ctx, r.x, r.y, r.w, r.h, radius, STLXTHEME_MENU_BG);
    stlxgfx_ctx_draw_rounded_rect(ctx, r.x, r.y, r.w, r.h, radius, STLXTHEME_RIM);
}

/* The copy glyph in a 14 px box: a sheet in front of the edges of another */
static void draw_copy_glyph(stlxgfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t color) {
    uint32_t half = (color >> 25) << 24 | (color & 0x00FFFFFF);
    stlxgfx_ctx_draw_rounded_rect(ctx, x + 4, y + 4, 8, 8, 2, color);
    stlxgfx_ctx_fill_rect(ctx, x + 4, y + 2, 5, 1, color);
    stlxgfx_ctx_fill_rect(ctx, x + 2, y + 4, 1, 5, color);
    stlxgfx_ctx_fill_rect(ctx, x + 3, y + 3, 1, 1, half);
}

int overlay_draw_copied(stlxgfx_surface_t *s, const char *figure) {
    if (open_faces() < 0) {
        return -1;
    }

    static const char lead[] = "Copied ";
    int32_t lead_w = stlxgfx_text_width(g_body, lead, sizeof(lead) - 1);
    int32_t figure_w = stlxgfx_text_width(g_medium, figure, strlen(figure));
    uint32_t w = (uint32_t)(PILL_PAD_LEFT + GLYPH_SIZE + PILL_GAP + lead_w + figure_w + PILL_PAD_RIGHT);
    overlay_rect_t pill = { ((int32_t)s->width - (int32_t)w) / 2, (int32_t)s->height - PILL_BOTTOM - PILL_HEIGHT,
                            w, PILL_HEIGHT };

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, s);
    draw_panel(&ctx, pill, STLXTHEME_MENU_RADIUS, PILL_RING);
    draw_copy_glyph(&ctx, pill.x + PILL_PAD_LEFT, pill.y + (PILL_HEIGHT - GLYPH_SIZE) / 2, STLXTHEME_TEXT_DIM);

    int32_t text_x = pill.x + PILL_PAD_LEFT + GLYPH_SIZE + PILL_GAP;
    draw_line_text(s, g_body, text_x, pill.y, PILL_HEIGHT, lead, STLXTHEME_TEXT_SECONDARY);
    draw_line_text(s, g_medium, text_x + lead_w, pill.y, PILL_HEIGHT, figure, STLXTHEME_TEXT);
    return 0;
}

/* Breaks text at its spaces into lines no wider than width. Returns how many, at most BODY_LINES_MAX. */
static int wrap_text(const stlxgfx_font *face, const char *text, int32_t width,
                     const char *starts[BODY_LINES_MAX], size_t lens[BODY_LINES_MAX]) {
    int count = 0;
    const char *line = text;
    while (*line && count < BODY_LINES_MAX) {
        size_t fit = strlen(line);
        if (stlxgfx_text_width(face, line, fit) > width) {
            const char *space = NULL;
            for (const char *p = line; *p; p++) {
                if (*p == ' ' && stlxgfx_text_width(face, line, (size_t)(p - line)) <= width) {
                    space = p;
                }
            }

            fit = space ? (size_t)(space - line) : fit;
        }

        starts[count] = line;
        lens[count] = fit;
        count++;

        line += fit;
        while (*line == ' ') {
            line++;
        }
    }

    return count;
}

static uint32_t button_width(const char *label) {
    return (uint32_t)(stlxgfx_text_width(g_medium, label, strlen(label)) + 2 * BUTTON_PAD_X + 2);
}

static void draw_button(stlxgfx_surface_t *s, stlxgfx_ctx_t *ctx, overlay_rect_t r, const char *label,
                        int is_default, int is_focused) {
    if (is_focused) {
        stlxgfx_ctx_draw_rounded_rect(ctx, r.x - 2, r.y - 2, r.w + 4, r.h + 4, STLXTHEME_CONTROL_RADIUS + 2,
                                      BUTTON_FOCUS_RING);
        stlxgfx_ctx_draw_rounded_rect(ctx, r.x - 1, r.y - 1, r.w + 2, r.h + 2, STLXTHEME_CONTROL_RADIUS + 1,
                                      BUTTON_FOCUS_RING);
    }

    stlxgfx_ctx_fill_rounded_rect(ctx, r.x, r.y, r.w, r.h, STLXTHEME_CONTROL_RADIUS,
                                  is_default ? BUTTON_DEFAULT : STLXTHEME_SURFACE);
    stlxgfx_ctx_draw_rounded_rect(ctx, r.x, r.y, r.w, r.h, STLXTHEME_CONTROL_RADIUS,
                                  is_default ? STLXTHEME_LINE_STRONG : STLXTHEME_LINE);

    int32_t label_w = stlxgfx_text_width(g_medium, label, strlen(label));
    draw_line_text(s, g_medium, r.x + ((int32_t)r.w - label_w) / 2, r.y, (int32_t)r.h, label, STLXTHEME_TEXT);
}

int overlay_draw_question(stlxgfx_surface_t *s, const overlay_question_t *q) {
    if (open_faces() < 0) {
        return -1;
    }

    int32_t width = q->width < (int32_t)s->width - 2 * SHEET_MARGIN ? q->width : (int32_t)s->width - 2 * SHEET_MARGIN;
    int32_t content_w = width - 2 * SHEET_PAD_X;

    const char *body_starts[BODY_LINES_MAX];
    size_t body_lens[BODY_LINES_MAX];
    int body_count = wrap_text(g_body, q->body, content_w, body_starts, body_lens);
    int preview_rows = q->preview_count + (q->preview_more[0] ? 1 : 0);
    int32_t preview_h = preview_rows > 0 ? PREVIEW_GAP + 2 * PREVIEW_PAD_Y + 2 + preview_rows * PREVIEW_LINE : 0;

    int32_t height = SHEET_PAD_TOP + TITLE_LINE + BODY_GAP + body_count * BODY_LINE + preview_h + SHEET_PAD_BOTTOM +
                     FOOTER_PAD_TOP + BUTTON_HEIGHT + FOOTER_PAD_BOTTOM;
    int32_t top = ((int32_t)s->height - height) / 3;
    overlay_rect_t panel = { ((int32_t)s->width - width) / 2, top > SHEET_MARGIN ? top : SHEET_MARGIN,
                             (uint32_t)width, (uint32_t)height };

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, s);
    stlxgfx_ctx_fill_rect(&ctx, 0, 0, s->width, s->height, SCRIM);
    draw_panel(&ctx, panel, STLXTHEME_WINDOW_RADIUS, SHEET_RING);
    stlxgfx_ctx_fill_rect(&ctx, panel.x + STLXTHEME_WINDOW_RADIUS, panel.y + 1,
                          panel.w - 2 * STLXTHEME_WINDOW_RADIUS, 1, SHEET_TOP_LIGHT);

    int32_t x = panel.x + SHEET_PAD_X;
    int32_t y = panel.y + SHEET_PAD_TOP;
    draw_line_text(s, g_title, x, y, TITLE_LINE, q->title, STLXTHEME_TEXT);
    y += TITLE_LINE + BODY_GAP;

    for (int i = 0; i < body_count; i++) {
        stlxgfx_draw_text(s, g_body, x, baseline_in(g_body, y, BODY_LINE), body_starts[i], body_lens[i],
                          STLXTHEME_TEXT_SECONDARY);
        y += BODY_LINE;
    }

    if (preview_rows > 0) {
        overlay_rect_t box = { x, y + PREVIEW_GAP, (uint32_t)content_w, (uint32_t)(preview_h - PREVIEW_GAP) };
        stlxgfx_ctx_fill_rounded_rect(&ctx, box.x, box.y, box.w, box.h, STLXTHEME_CARD_RADIUS, STLXTHEME_CARD);
        stlxgfx_ctx_draw_rounded_rect(&ctx, box.x, box.y, box.w, box.h, STLXTHEME_CARD_RADIUS, STLXTHEME_CARD_LINE);

        int32_t number_right = box.x + 1 + PREVIEW_PAD_X + NUMBER_WIDTH;
        int32_t text_room = box.x + (int32_t)box.w - 1 - PREVIEW_PAD_X - (number_right + NUMBER_GAP);
        int32_t row = box.y + 1 + PREVIEW_PAD_Y;
        for (int i = 0; i < q->preview_count; i++) {
            char number[4] = { (char)('1' + i), '\0' };
            int32_t number_w = stlxgfx_text_width(g_mono, number, 1);
            draw_line_text(s, g_mono, number_right - number_w, row, PREVIEW_LINE, number, STLXTHEME_TEXT_DISABLED);
            draw_fitted_text(s, g_mono, number_right + NUMBER_GAP, row, PREVIEW_LINE, q->preview[i], text_room,
                             STLXTHEME_TERMINAL_TEXT);
            row += PREVIEW_LINE;
        }

        if (q->preview_more[0]) {
            draw_line_text(s, g_mono, number_right + NUMBER_GAP, row, PREVIEW_LINE, q->preview_more,
                           STLXTHEME_TEXT_DIM);
        }

        y += preview_h;
    }

    y += SHEET_PAD_BOTTOM + FOOTER_PAD_TOP;
    uint32_t accept_w = button_width(q->accept);
    uint32_t cancel_w = button_width(q->cancel);

    g_accept_rect = (overlay_rect_t){ panel.x + width - FOOTER_PAD_X - (int32_t)accept_w, y, accept_w,
                                      BUTTON_HEIGHT };
    g_cancel_rect = (overlay_rect_t){ g_accept_rect.x - BUTTON_GAP - (int32_t)cancel_w, y, cancel_w,
                                      BUTTON_HEIGHT };
    draw_button(s, &ctx, g_cancel_rect, q->cancel, 0, !q->is_accept_focused);
    draw_button(s, &ctx, g_accept_rect, q->accept, 1, q->is_accept_focused);

    if (q->note[0]) {
        draw_line_text(s, g_caption, panel.x + FOOTER_PAD_X + NOTE_INSET, y, BUTTON_HEIGHT, q->note,
                       STLXTHEME_TEXT_DIM);
    }

    return 0;
}

static int contains(overlay_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + (int32_t)r.w && y < r.y + (int32_t)r.h;
}

overlay_hit_t overlay_question_hit(int32_t x, int32_t y) {
    if (contains(g_accept_rect, x, y)) {
        return OVERLAY_HIT_ACCEPT;
    }

    if (contains(g_cancel_rect, x, y)) {
        return OVERLAY_HIT_CANCEL;
    }

    return OVERLAY_HIT_NONE;
}
