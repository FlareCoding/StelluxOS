#include "keycap.hpp"

#include "chord.hpp"

#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstring>

constexpr int32_t KEYCAP_PADDING_X = 5;
constexpr float KEYCAP_TEXT_SIZE = 11.0f;

keycap::keycap(const std::string& name)
    : m_text(name, STLXTHEME_MONO_FONT_PATH, KEYCAP_TEXT_SIZE, STLXTHEME_TEXT_DIM),
      m_width(std::max(KEYCAP_HEIGHT, m_text.width() + 2 * KEYCAP_PADDING_X)),
      m_ring(make_ring_picture(m_width, KEYCAP_HEIGHT, STLXTHEME_CHIP_RADIUS, STLXTHEME_LINE)),
      m_hover_ring(make_ring_picture(m_width, KEYCAP_HEIGHT, STLXTHEME_CHIP_RADIUS, STLXTHEME_LINE_STRONG)) {}

keycap::~keycap() {
    stlxgfx_destroy_surface(m_ring);
    stlxgfx_destroy_surface(m_hover_ring);
}

void keycap::set_hover(bool hover) {
    m_hover = hover;
    m_text.set_color(hover ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT_DIM);
}

void keycap::paint(ui::painter& p, int32_t x, int32_t top) const {
    p.rounded_rect({ x, top, m_width, KEYCAP_HEIGHT }, STLXTHEME_CHIP_RADIUS, STLXTHEME_SURFACE);
    p.image({ x, top }, m_hover ? m_hover_ring : m_ring);
    m_text.draw(p, x + (m_width - m_text.width()) / 2, top, KEYCAP_HEIGHT);
}

std::vector<std::string> shortcut_keys(const stlxconf_t& conf, stlxconf_action_kind_t kind, const char* path,
                                       const char* args) {
    for (uint32_t i = 0; i < conf.shortcut_count; i++) {
        const stlxconf_shortcut_t& shortcut = conf.shortcuts[i];
        bool same_kind = stlxconf_action_kind(shortcut.action) == kind;
        bool same_target = strcmp(shortcut.path, path) == 0 && strcmp(shortcut.args, args) == 0;
        bool has_target = kind == STLXCONF_ACTION_EXEC || kind == STLXCONF_ACTION_SCREENSHOT;
        if (!same_kind || (has_target && !same_target)) {
            continue;
        }

        std::vector<std::string> keys = chord_keycaps(shortcut.key);
        if (!keys.empty()) {
            return keys;
        }
    }

    return {};
}
