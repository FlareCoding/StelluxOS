#include "popover.hpp"

#include "../decor.hpp"
#include "../material.hpp"

#include <stlxgfx/blend.h>
#include <stlxtheme/theme.h>

#include <cstdio>
#include <ctime>

/* The output row a panel's top edge lies on, a few pixels under the bar */
constexpr int32_t PANEL_TOP = STLXTHEME_TOP_BAR_HEIGHT + 4;

constexpr int32_t PANEL_HEIGHT_MAX = 2048;
constexpr uint64_t OPEN_NS = 140000000ull;
constexpr uint64_t CLOSE_NS = 100000000ull;

/* How far above its resting row a panel's opening starts and its closing ends */
constexpr int32_t SLIDE_PX = 6;
constexpr uint32_t OPAQUE = 255;

namespace {

/* The panel's glass and rim under its content */
class panel_frame : public ui::box {
public:
    explicit panel_frame(const stlxgfx_surface_t* background) : m_background(background) {}

    void hold(std::unique_ptr<ui::widget> content) {
        content->s().main = ui::length::flex();
        adopt(std::move(content));
    }

    void paint(ui::painter& p) override {
        p.image({ 0, 0 }, m_background);
    }

private:
    const stlxgfx_surface_t* m_background;
};

} // namespace

/* A layer of the focused window's shadow, its alpha in 255ths rounded to the nearest */
static constexpr shadow_layer focused_layer(size_t i) {
    const decor::window_shadow_layer& l = decor::FOCUSED_SHADOW[i];
    return { l.offset_y, l.blur, static_cast<uint32_t>((l.alpha_percent * 255 + 50) / 100) };
}

constexpr shadow_layer WIDEST_LAYER = focused_layer(3);

/* The room the widest layer takes beside, above and below a panel */
constexpr int32_t SHADOW_SIDE = WIDEST_LAYER.blur;
constexpr int32_t SHADOW_TOP = WIDEST_LAYER.blur - WIDEST_LAYER.offset_y;
constexpr int32_t SHADOW_BOTTOM = WIDEST_LAYER.blur + WIDEST_LAYER.offset_y;
constexpr uint32_t SHADOW_CONTACT_ALPHA = focused_layer(0).alpha;

static const shadow_layer POPOVER_SHADOW[] = { focused_layer(1), focused_layer(2), focused_layer(3) };

static const panel_style POPOVER_STYLE = {
    STLXTHEME_WINDOW_RADIUS, STLXTHEME_RIM, material::PANEL_RIM_FADE, SHADOW_CONTACT_ALPHA, POPOVER_SHADOW,
    sizeof(POPOVER_SHADOW) / sizeof(POPOVER_SHADOW[0]), SHADOW_SIDE, SHADOW_TOP, SHADOW_BOTTOM,
};

static uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

/* Fast at first, settling at the end */
static float ease_out(float t) {
    float rest = 1.0f - t;
    return 1.0f - rest * rest * rest;
}

void blend_popover_layer(stlxgfx_surface_t* back, const damage_list::rect& clip, const popover_layer& shown) {
    if (!shown.surface) {
        return;
    }

    int32_t w = static_cast<int32_t>(shown.surface->width);
    int32_t h = static_cast<int32_t>(shown.surface->height);
    damage_list::rect area = damage_list::intersect({ shown.x, shown.y, w, h }, clip);
    for (int32_t y = area.y; y < area.y + area.h; y++) {
        size_t source_row = static_cast<size_t>(y - shown.y) * shown.surface->pitch;
        const uint32_t* source = reinterpret_cast<const uint32_t*>(shown.surface->pixels + source_row);
        uint32_t* target = reinterpret_cast<uint32_t*>(back->pixels + static_cast<size_t>(y) * back->pitch);
        for (int32_t x = area.x; x < area.x + area.w; x++) {
            uint32_t pixel = source[x - shown.x];
            uint32_t alpha = stlxgfx_div255((pixel >> 24) * shown.opacity);
            target[x] = decor::blend_over(target[x], pixel, alpha);
        }
    }
}

layer_piece popover_layer_piece(const popover_layer& shown) {
    if (!shown.surface || shown.address == 0) {
        return {};
    }

    layer_piece piece;
    piece.bounds = { shown.x, shown.y, static_cast<int32_t>(shown.surface->width),
                     static_cast<int32_t>(shown.surface->height) };
    piece.picture = { shown.address, shown.surface->pitch, shown.x, shown.y };
    piece.opacity = shown.opacity;

    return piece;
}

void popover_host::use_picture_maker(picture_maker* maker) {
    close_at_once();
    m_picture.drop();
    m_maker = maker;
}

void popover_host::open(popover_panel& panel, const stlxgfx_surface_t* frost) {
    close_at_once();

    std::unique_ptr<ui::widget> content = panel.build();
    int32_t width = panel.width();
    int32_t height = content->measure({ width, PANEL_HEIGHT_MAX }).h;
    int32_t left = panel.left(m_output_w);
    if (!m_picture.make(m_maker, POPOVER_STYLE, width, height, left, PANEL_TOP, frost)) {
        printf("stlxdm: the popover's picture could not be made, memory ran out\r\n");
        content.reset();
        panel.closed();
        return;
    }

    auto frame = std::make_unique<panel_frame>(m_picture.background());
    frame->hold(std::move(content));
    m_host.set_root(std::move(frame));

    m_panel = &panel;
    m_left = left;
    m_phase = phase::opening;
    m_phase_start_ns = monotonic_ns();
    m_opacity = 0;
    m_slide = -SLIDE_PX;
}

/* A close during the opening fades from the opacity it reached */
void popover_host::close() {
    if (m_phase != phase::opening && m_phase != phase::open) {
        return;
    }

    uint64_t faded_ns = CLOSE_NS * (OPAQUE - m_opacity) / OPAQUE;
    m_phase = phase::closing;
    m_phase_start_ns = monotonic_ns() - faded_ns;
}

void popover_host::close_now() {
    close();
    if (m_phase == phase::closing) {
        m_phase_start_ns = monotonic_ns() - CLOSE_NS;
    }
}

popover_panel* popover_host::panel() const {
    return m_phase == phase::opening || m_phase == phase::open ? m_panel : nullptr;
}

bool popover_host::dirty() const {
    bool has_owed = m_owed.w > 0 && m_owed.h > 0;
    return has_owed || (m_panel && (m_phase != phase::open || m_host.dirty()));
}

void popover_host::flush(damage_list& damage) {
    if (m_owed.w > 0 && m_owed.h > 0) {
        damage.add(m_owed.x, m_owed.y, m_owed.w, m_owed.h);
        m_owed = {};
    }

    if (!m_panel) {
        return;
    }

    uint64_t elapsed_ns = monotonic_ns() - m_phase_start_ns;
    if (m_phase == phase::closing && elapsed_ns >= CLOSE_NS) {
        close_at_once();
        damage.add(m_owed.x, m_owed.y, m_owed.w, m_owed.h);
        m_owed = {};
        return;
    }

    if (m_phase == phase::opening && elapsed_ns >= OPEN_NS) {
        m_phase = phase::open;
    }

    float shown = 1.0f;
    if (m_phase == phase::opening) {
        shown = ease_out(static_cast<float>(elapsed_ns) / static_cast<float>(OPEN_NS));
    } else if (m_phase == phase::closing) {
        shown = 1.0f - static_cast<float>(elapsed_ns) / static_cast<float>(CLOSE_NS);
    }

    m_opacity = static_cast<uint32_t>(shown * static_cast<float>(OPAQUE) + 0.5f);
    m_slide = -static_cast<int32_t>((1.0f - shown) * static_cast<float>(SLIDE_PX) + 0.5f);
    paint_tree(damage);

    damage_list::rect at = bounds();
    bool moved = at.x != m_shown.x || at.y != m_shown.y || at.w != m_shown.w || at.h != m_shown.h;
    if (moved || m_opacity != m_shown_opacity) {
        damage_list::rect changed = damage_list::union_of(m_shown, at);
        damage.add(changed.x, changed.y, changed.w, changed.h);
    }

    m_shown = at;
    m_shown_opacity = m_opacity;
}

popover_layer popover_host::layer() const {
    if (!m_panel) {
        return {};
    }

    damage_list::rect at = bounds();
    return { m_picture.surface(), m_picture.address(), at.x, at.y, m_opacity };
}

bool popover_host::contains(int32_t x, int32_t y) const {
    if (!panel()) {
        return false;
    }

    ui::rect box = m_picture.box();
    ui::rect on_output = { m_left, PANEL_TOP + m_slide, box.w, box.h };

    return on_output.contains({ x, y });
}

void popover_host::pointer_move(int32_t x, int32_t y) {
    if (m_panel) {
        m_host.pointer_move(x - m_left, y - PANEL_TOP - m_slide);
    }
}

void popover_host::pointer_button(int32_t x, int32_t y, uint8_t button, bool down) {
    if (m_panel) {
        m_host.pointer_button(x - m_left, y - PANEL_TOP - m_slide, button, down);
    }
}

void popover_host::refresh() {
    if (panel()) {
        m_panel->refresh();
    }
}

/* Takes the panel down without a fade, owing its region to the next flush */
void popover_host::close_at_once() {
    if (!m_panel) {
        return;
    }

    popover_panel* taken_down = m_panel;
    m_owed = damage_list::union_of(m_owed, m_shown);
    m_host.set_root(nullptr);
    m_panel = nullptr;
    m_phase = phase::closed;

    m_shown = {};
    m_shown_opacity = 0;
    m_opacity = 0;
    taken_down->closed();
}

void popover_host::paint_tree(damage_list& damage) {
    if (!m_host.dirty()) {
        return;
    }

    ui::rect box = m_picture.box();
    std::vector<ui::rect> painted;
    m_host.layout_now(box.w, box.h);
    m_host.paint_now(m_picture.content(), painted);

    m_picture.round_corners(painted);

    damage_list::rect at = bounds();
    for (const ui::rect& r : painted) {
        damage.add(at.x + box.x + r.x, at.y + box.y + r.y, r.w, r.h);
    }
}

/* The picture's place on the output, its shadow included */
damage_list::rect popover_host::bounds() const {
    ui::rect box = m_picture.box();
    int32_t w = static_cast<int32_t>(m_picture.surface()->width);
    int32_t h = static_cast<int32_t>(m_picture.surface()->height);

    return { m_left - box.x, PANEL_TOP - box.y + m_slide, w, h };
}
