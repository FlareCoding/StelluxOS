#include "stlxdm_taskbar.h"
#include <stlxgfx/font.h>
#include <string.h>

void stlxdm_taskbar_init(stlxdm_taskbar_t* tb) {
    memset(tb, 0, sizeof(*tb));
    tb->hover_index = -1;
    tb->press_index = -1;
    tb->launch_index = -1;
}

void stlxdm_taskbar_load(stlxdm_taskbar_t* tb, const stlxdm_conf_t* conf,
                          uint32_t screen_w, uint32_t screen_h) {
    stlxdm_taskbar_init(tb);

    for (int i = 0; i < conf->taskbar_count && i < STLXDM_TASKBAR_MAX_ITEMS; i++) {
        strncpy(tb->items[i].label, conf->taskbar[i].label,
                sizeof(tb->items[i].label) - 1);
        strncpy(tb->items[i].path, conf->taskbar[i].path,
                sizeof(tb->items[i].path) - 1);
        tb->count++;
    }

    if (tb->count == 0) return;

    uint32_t icons_w = (uint32_t)tb->count * STLXDM_TASKBAR_ICON_SIZE
                     + (uint32_t)(tb->count - 1) * STLXDM_TASKBAR_PADDING;
    tb->dock_w = icons_w + 2 * STLXDM_TASKBAR_DOCK_PADDING;
    tb->dock_h = STLXDM_TASKBAR_ICON_SIZE + 2 * STLXDM_TASKBAR_DOCK_PADDING;
    tb->dock_x = ((int32_t)screen_w - (int32_t)tb->dock_w) / 2;
    tb->dock_y = (int32_t)screen_h - (int32_t)tb->dock_h
                 - STLXDM_TASKBAR_MARGIN_BOTTOM;

    int32_t ix = tb->dock_x + STLXDM_TASKBAR_DOCK_PADDING;
    int32_t iy = tb->dock_y + STLXDM_TASKBAR_DOCK_PADDING;
    for (int i = 0; i < tb->count; i++) {
        tb->items[i].x = ix;
        tb->items[i].y = iy;
        ix += STLXDM_TASKBAR_ICON_SIZE + STLXDM_TASKBAR_PADDING;
    }
}

static void draw_default_icon(stlxgfx_ctx_t* ctx, int32_t x, int32_t y,
                               uint32_t bg, const char* label) {
    stlxgfx_ctx_fill_rounded_rect(ctx, x, y,
                                   STLXDM_TASKBAR_ICON_SIZE,
                                   STLXDM_TASKBAR_ICON_SIZE,
                                   STLXDM_TASKBAR_ICON_RADIUS, bg);

    char letter[2] = { 0, 0 };
    if (label[0] != '\0') {
        letter[0] = label[0];
        if (letter[0] >= 'a' && letter[0] <= 'z')
            letter[0] -= 32;
    } else {
        letter[0] = '?';
    }

    uint32_t font_size = 18;
    uint32_t tw = 0, th = 0;
    stlxgfx_ctx_text_size(letter, font_size, &tw, &th);
    int32_t tx = x + ((int32_t)STLXDM_TASKBAR_ICON_SIZE - (int32_t)tw) / 2;
    int32_t ty = y + ((int32_t)STLXDM_TASKBAR_ICON_SIZE - (int32_t)th) / 2;
    stlxgfx_ctx_draw_text(ctx, tx, ty, letter, font_size,
                           STLXDM_TASKBAR_ICON_TEXT_COLOR);
}

static void draw_tooltip(stlxgfx_ctx_t* ctx, int32_t anchor_x, int32_t anchor_y,
                          const char* text) {
    uint32_t tw = 0, th = 0;
    stlxgfx_ctx_text_size(text, STLXDM_TASKBAR_TOOLTIP_FONT, &tw, &th);

    uint32_t box_w = tw + 2 * STLXDM_TASKBAR_TOOLTIP_PAD_X;
    uint32_t box_h = th + 2 * STLXDM_TASKBAR_TOOLTIP_PAD_Y;

    int32_t bx = anchor_x + (int32_t)STLXDM_TASKBAR_ICON_SIZE / 2
                 - (int32_t)box_w / 2;
    int32_t by = anchor_y - (int32_t)box_h - STLXDM_TASKBAR_TOOLTIP_GAP;

    stlxgfx_ctx_fill_rounded_rect(ctx, bx - 1, by - 1,
                                   box_w + 2, box_h + 2,
                                   STLXDM_TASKBAR_TOOLTIP_RAD + 1,
                                   STLXDM_TASKBAR_TOOLTIP_BORDER);

    stlxgfx_ctx_fill_rounded_rect(ctx, bx, by, box_w, box_h,
                                   STLXDM_TASKBAR_TOOLTIP_RAD,
                                   STLXDM_TASKBAR_TOOLTIP_BG);

    int32_t text_x = bx + (int32_t)STLXDM_TASKBAR_TOOLTIP_PAD_X;
    int32_t text_y = by + (int32_t)STLXDM_TASKBAR_TOOLTIP_PAD_Y;
    stlxgfx_ctx_draw_text(ctx, text_x, text_y, text,
                           STLXDM_TASKBAR_TOOLTIP_FONT,
                           STLXDM_TASKBAR_TOOLTIP_TEXT);
}

void stlxdm_taskbar_draw(stlxdm_taskbar_t* tb, stlxgfx_ctx_t* ctx) {
    if (tb->count == 0) return;

    /* Dock border */
    stlxgfx_ctx_fill_rounded_rect(ctx,
        tb->dock_x - 1, tb->dock_y - 1,
        tb->dock_w + 2, tb->dock_h + 2,
        STLXDM_TASKBAR_DOCK_RADIUS + 1,
        STLXDM_TASKBAR_DOCK_BORDER);

    /* Dock background */
    stlxgfx_ctx_fill_rounded_rect(ctx,
        tb->dock_x, tb->dock_y,
        tb->dock_w, tb->dock_h,
        STLXDM_TASKBAR_DOCK_RADIUS,
        STLXDM_TASKBAR_DOCK_BG);

    /* Icon buttons */
    for (int i = 0; i < tb->count; i++) {
        uint32_t bg = STLXDM_TASKBAR_ICON_BG;
        if (i == tb->press_index)
            bg = STLXDM_TASKBAR_ICON_PRESS_BG;
        else if (i == tb->hover_index)
            bg = STLXDM_TASKBAR_ICON_HOVER_BG;

        draw_default_icon(ctx, tb->items[i].x, tb->items[i].y,
                          bg, tb->items[i].label);
    }

    /* Tooltip for hovered item */
    if (tb->hover_index >= 0 && tb->hover_index < tb->count) {
        stlxdm_taskbar_item_t* item = &tb->items[tb->hover_index];
        draw_tooltip(ctx, item->x, item->y, item->label);
    }
}

int stlxdm_taskbar_hit_test(const stlxdm_taskbar_t* tb,
                              int32_t px, int32_t py) {
    for (int i = 0; i < tb->count; i++) {
        int32_t ix = tb->items[i].x;
        int32_t iy = tb->items[i].y;
        if (px >= ix && px < ix + (int32_t)STLXDM_TASKBAR_ICON_SIZE &&
            py >= iy && py < iy + (int32_t)STLXDM_TASKBAR_ICON_SIZE) {
            return i;
        }
    }
    return -1;
}

int stlxdm_taskbar_contains(const stlxdm_taskbar_t* tb,
                               int32_t px, int32_t py) {
    if (tb->count == 0) return 0;
    return px >= tb->dock_x &&
           px < tb->dock_x + (int32_t)tb->dock_w &&
           py >= tb->dock_y &&
           py < tb->dock_y + (int32_t)tb->dock_h;
}

void stlxdm_taskbar_set_hover(stlxdm_taskbar_t* tb, int32_t px, int32_t py) {
    tb->hover_index = stlxdm_taskbar_hit_test(tb, px, py);
}
