#include "stlxdm_taskbar.h"
#include <stlxgfx/bmp.h>
#include <string.h>

#define TB_ITEM_BG          0xFF313244
#define TB_ITEM_HOVER_BG    0xFF45475A
#define TB_ITEM_HOVER_BORDER 0xFF585B70
#define TB_ITEM_RADIUS      6
#define TB_DEFAULT_ICON_COLOR 0xFF6C7086
#define TB_TOOLTIP_BG       0xFF45475A
#define TB_TOOLTIP_TEXT      0xFFCDD6F4
#define TB_TOOLTIP_RADIUS   4
#define TB_TOOLTIP_PAD_H    8
#define TB_TOOLTIP_PAD_V    4
#define TB_TOOLTIP_GAP      6
#define TB_TOOLTIP_FONT     12

static const uint32_t g_item_accent_colors[] = {
    0xFF89B4FA, 0xFFF38BA8, 0xFFA6E3A1, 0xFFFAB387,
    0xFFCBA6F7, 0xFF94E2D5, 0xFFF9E2AF, 0xFFEBA0AC,
    0xFF74C7EC, 0xFFB4BEFE, 0xFFF2CDCD, 0xFFF5C2E7,
    0xFF89DCEB, 0xFFBAC2DE, 0xFFCDD6F4, 0xFFFAB387
};

void stlxdm_taskbar_init(stlxdm_taskbar_t* tb, const stlxdm_config_t* conf,
                          uint32_t fb_width, uint32_t fb_height) {
    memset(tb, 0, sizeof(*tb));
    tb->conf = conf;
    tb->fb_width = fb_width;
    tb->fb_height = fb_height;
    tb->bar_y = (int32_t)(fb_height - conf->taskbar_height);
    tb->hover_index = -1;
    tb->press_index = -1;
    tb->launch_path[0] = '\0';

    tb->default_icon = stlxgfx_load_bmp(STLXDM_CONF_DEFAULT_ICON_PATH);

    for (int i = 0; i < conf->taskbar_item_count; i++) {
        const char* icon_path = conf->taskbar_items[i].icon_path;
        if (icon_path[0])
            tb->icon_surfaces[i] = stlxgfx_load_bmp(icon_path);
    }
}

void stlxdm_taskbar_cleanup(stlxdm_taskbar_t* tb) {
    for (int i = 0; i < STLXDM_CONF_MAX_TASKBAR_ITEMS; i++) {
        if (tb->icon_surfaces[i]) {
            stlxgfx_destroy_surface(tb->icon_surfaces[i]);
            tb->icon_surfaces[i] = NULL;
        }
    }
    if (tb->default_icon) {
        stlxgfx_destroy_surface(tb->default_icon);
        tb->default_icon = NULL;
    }
}

static void taskbar_item_rect(const stlxdm_taskbar_t* tb, int idx,
                               int32_t* out_x, int32_t* out_y,
                               uint32_t* out_w, uint32_t* out_h) {
    const stlxdm_config_t* c = tb->conf;
    int count = c->taskbar_item_count;
    uint32_t icon = c->taskbar_icon_size;
    uint32_t spacing = c->taskbar_spacing;

    uint32_t total_w = (uint32_t)count * icon;
    if (count > 1) total_w += (uint32_t)(count - 1) * spacing;

    int32_t start_x = ((int32_t)tb->fb_width - (int32_t)total_w) / 2;
    int32_t item_y = tb->bar_y +
        (int32_t)(c->taskbar_height - icon) / 2;

    *out_x = start_x + idx * (int32_t)(icon + spacing);
    *out_y = item_y;
    *out_w = icon;
    *out_h = icon;
}

static int item_index_at(const stlxdm_taskbar_t* tb,
                          int32_t px, int32_t py) {
    for (int i = 0; i < tb->conf->taskbar_item_count; i++) {
        int32_t ix, iy;
        uint32_t iw, ih;
        taskbar_item_rect(tb, i, &ix, &iy, &iw, &ih);
        if (px >= ix && px < ix + (int32_t)iw &&
            py >= iy && py < iy + (int32_t)ih) {
            return i;
        }
    }
    return -1;
}

static void draw_default_icon(stlxgfx_ctx_t* ctx, int32_t x, int32_t y,
                               uint32_t size, uint32_t color) {
    uint32_t font_size = size * 2 / 3;
    if (font_size < 10) font_size = 10;

    uint32_t tw = 0, th = 0;
    stlxgfx_ctx_text_size("?", font_size, &tw, &th);
    int32_t tx = x + ((int32_t)size - (int32_t)tw) / 2;
    int32_t ty = y + ((int32_t)size - (int32_t)th) / 2;
    stlxgfx_ctx_draw_text(ctx, tx, ty, "?", font_size, color);
}

static void draw_label_icon(stlxgfx_ctx_t* ctx, int32_t x, int32_t y,
                             uint32_t size, const char* label,
                             uint32_t color) {
    if (!label[0]) {
        draw_default_icon(ctx, x, y, size, TB_DEFAULT_ICON_COLOR);
        return;
    }

    char glyph[2] = { label[0], '\0' };
    uint32_t font_size = size * 2 / 3;
    if (font_size < 10) font_size = 10;

    uint32_t tw = 0, th = 0;
    stlxgfx_ctx_text_size(glyph, font_size, &tw, &th);
    int32_t tx = x + ((int32_t)size - (int32_t)tw) / 2;
    int32_t ty = y + ((int32_t)size - (int32_t)th) / 2;
    stlxgfx_ctx_draw_text(ctx, tx, ty, glyph, font_size, color);
}

void stlxdm_taskbar_draw(stlxdm_taskbar_t* tb, stlxgfx_ctx_t* ctx) {
    const stlxdm_config_t* c = tb->conf;
    if (c->taskbar_item_count <= 0) return;

    stlxgfx_ctx_fill_rect(ctx, 0, tb->bar_y,
                           tb->fb_width, c->taskbar_height,
                           c->bar_color);
    stlxgfx_ctx_fill_rect(ctx, 0, tb->bar_y,
                           tb->fb_width, 1, c->accent_color);

    for (int i = 0; i < c->taskbar_item_count; i++) {
        int32_t ix, iy;
        uint32_t iw, ih;
        taskbar_item_rect(tb, i, &ix, &iy, &iw, &ih);

        int hovered = (i == tb->hover_index);
        int pressed = (i == tb->press_index);

        uint32_t bg = pressed  ? 0xFF3E3E52
                    : hovered  ? TB_ITEM_HOVER_BG
                    :            TB_ITEM_BG;

        if (hovered && !pressed) {
            stlxgfx_ctx_fill_rounded_rect(ctx, ix - 1, iy - 1,
                                           iw + 2, ih + 2,
                                           TB_ITEM_RADIUS + 1,
                                           TB_ITEM_HOVER_BORDER);
        }

        stlxgfx_ctx_fill_rounded_rect(ctx, ix, iy, iw, ih,
                                       TB_ITEM_RADIUS, bg);

        uint32_t accent = g_item_accent_colors[
            (unsigned)i % (sizeof(g_item_accent_colors) /
                           sizeof(g_item_accent_colors[0]))];

        const stlxdm_conf_taskbar_item_t* it = &c->taskbar_items[i];
        stlxgfx_surface_t* icon = tb->icon_surfaces[i];
        if (!icon) icon = tb->default_icon;

        if (icon) {
            stlxgfx_ctx_blit(ctx, ix, iy, icon, 0, 0, iw, ih);
        } else if (it->label[0]) {
            draw_label_icon(ctx, ix, iy, iw, it->label, accent);
        } else {
            draw_default_icon(ctx, ix, iy, iw, TB_DEFAULT_ICON_COLOR);
        }
    }

    if (tb->hover_index >= 0 &&
        tb->hover_index < c->taskbar_item_count) {
        const stlxdm_conf_taskbar_item_t* it =
            &c->taskbar_items[tb->hover_index];
        if (it->label[0]) {
            uint32_t tw = 0, th = 0;
            stlxgfx_ctx_text_size(it->label, TB_TOOLTIP_FONT, &tw, &th);

            int32_t ix, iy;
            uint32_t iw, ih;
            taskbar_item_rect(tb, tb->hover_index, &ix, &iy, &iw, &ih);

            uint32_t tip_w = tw + 2 * TB_TOOLTIP_PAD_H;
            uint32_t tip_h = th + 2 * TB_TOOLTIP_PAD_V;
            int32_t tip_x = ix + (int32_t)(iw / 2) - (int32_t)(tip_w / 2);
            int32_t tip_y = iy - (int32_t)tip_h - TB_TOOLTIP_GAP;

            if (tip_x < 2) tip_x = 2;
            if (tip_x + (int32_t)tip_w > (int32_t)tb->fb_width - 2)
                tip_x = (int32_t)tb->fb_width - (int32_t)tip_w - 2;
            if (tip_y < 2) tip_y = 2;

            stlxgfx_ctx_fill_rounded_rect(ctx, tip_x, tip_y,
                                           tip_w, tip_h,
                                           TB_TOOLTIP_RADIUS,
                                           TB_TOOLTIP_BG);
            stlxgfx_ctx_draw_text(ctx,
                                   tip_x + TB_TOOLTIP_PAD_H,
                                   tip_y + TB_TOOLTIP_PAD_V,
                                   it->label, TB_TOOLTIP_FONT,
                                   TB_TOOLTIP_TEXT);
        }
    }
}

int stlxdm_taskbar_hit_test(const stlxdm_taskbar_t* tb,
                             int32_t px, int32_t py) {
    (void)px;
    if (tb->conf->taskbar_item_count <= 0) return 0;
    return py >= tb->bar_y;
}

void stlxdm_taskbar_on_press(stlxdm_taskbar_t* tb,
                              int32_t px, int32_t py) {
    tb->press_index = item_index_at(tb, px, py);
}

void stlxdm_taskbar_on_release(stlxdm_taskbar_t* tb,
                                int32_t px, int32_t py) {
    if (tb->press_index >= 0) {
        int release_idx = item_index_at(tb, px, py);
        if (release_idx == tb->press_index) {
            const stlxdm_conf_taskbar_item_t* it =
                &tb->conf->taskbar_items[release_idx];
            if (it->path[0]) {
                size_t len = strlen(it->path);
                if (len >= sizeof(tb->launch_path))
                    len = sizeof(tb->launch_path) - 1;
                memcpy(tb->launch_path, it->path, len);
                tb->launch_path[len] = '\0';
            }
        }
    }
    tb->press_index = -1;
}

void stlxdm_taskbar_update_hover(stlxdm_taskbar_t* tb,
                                  int32_t px, int32_t py) {
    if (py >= tb->bar_y)
        tb->hover_index = item_index_at(tb, px, py);
    else
        tb->hover_index = -1;
}
