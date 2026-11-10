/* The compositor's panels on its own toolkit hosts. The bar's and the dock's trees paint straight into the backdrop
 * rows they own, so compose needs no step for them, and the popover's tree paints into a picture above the windows. */
#include "panels.hpp"
#include "material.hpp"

#include <stlx/net.h>
#include <stlxgfx/image.h>
#include <stlxtheme/theme.h>

#include <cstdio>
#include <cstring>
#include <ctime>

constexpr uint32_t PIN_BG = 0xFF313244;
constexpr uint32_t PIN_BG_HOVER = 0xFF45475A;
constexpr uint32_t PIN_BG_PRESS = 0xFF3E3E52;
constexpr uint32_t PIN_RING = 0xFF585B70;
constexpr int32_t PIN_RADIUS = 6;
constexpr int32_t PIN_ICON_RADIUS = 5;
constexpr uint32_t PIN_FALLBACK_FG = 0xFF6C7086;

constexpr uint32_t TIP_BG = 0xFF45475A;
constexpr uint32_t TIP_FG = 0xFFCDD6F4;
constexpr int32_t TIP_RADIUS = 4;
constexpr int32_t TIP_PAD_H = 8;
constexpr int32_t TIP_PAD_V = 4;
constexpr int32_t TIP_GAP = 6;

/* The tooltip pops above the dock, so hover transitions damage this
 * far beyond the band */
constexpr int32_t TIP_REACH = 48;

/* The tick runs on every wakeup, so it asks for the rarely changing network state only this often */
constexpr uint64_t NET_POLL_NS = 3000000000ull;

/* Letter-glyph fallback colors for pins without an icon file */
static const uint32_t PIN_ACCENTS[] = {
    0xFF89B4FA, 0xFFF38BA8, 0xFFA6E3A1, 0xFFFAB387,
    0xFFCBA6F7, 0xFF94E2D5, 0xFFF9E2AF, 0xFFEBA0AC,
    0xFF74C7EC, 0xFFB4BEFE, 0xFFF2CDCD, 0xFFF5C2E7,
    0xFF89DCEB, 0xFFBAC2DE, 0xFFCDD6F4, 0xFFFAB387
};

namespace {

/* A row on a picture of the glass beneath it, so repainting a child
 * repaints the glass under it first */
class glass_row : public ui::box {
public:
    explicit glass_row(const stlxgfx_surface_t* glass) : ui::box(ui::axis::row), m_glass(glass) {}

    void paint(ui::painter& p) override {
        p.image({ 0, 0 }, m_glass);
    }

private:
    const stlxgfx_surface_t* m_glass;
};

/* One pinned launcher: an icon tile on a rounded hover surface,
 * firing its launch callback on release inside */
class dock_button : public ui::widget {
public:
    dock_button(stlxgfx_surface_t* icon, const stlxconf_pin_t& pin,
                int32_t index, uint32_t accent, int32_t icon_px)
        : m_icon(icon), m_pin(&pin), m_index(index), m_accent(accent),
          m_icon_px(icon_px) {}

    std::function<void(const stlxconf_pin_t&)> on_launch;
    std::function<void(int32_t, bool)> on_hover;

    ui::size measure(ui::size) override {
        return { m_icon_px + 2, m_icon_px + 2 };
    }

    void paint(ui::painter& p) override {
        ui::rect icon_box = { 1, 1, m_icon_px, m_icon_px };

        if (m_hover && !m_pressed) {
            p.rounded_rect({ 0, 0, m_frame.w, m_frame.h },
                           PIN_RADIUS + 1, PIN_RING);
        }
        p.rounded_rect(icon_box, PIN_RADIUS,
                       m_pressed ? PIN_BG_PRESS
                       : m_hover ? PIN_BG_HOVER : PIN_BG);

        if (m_icon) {
            p.image({ icon_box.x, icon_box.y }, m_icon, PIN_ICON_RADIUS);
            return;
        }

        /* Fallback glyph: the label's first letter, or a question
         * mark when there is no label either */
        char glyph[2] = { m_pin->label[0] ? m_pin->label[0] : '?', '\0' };
        uint32_t px = static_cast<uint32_t>(m_icon_px) * 2 / 3;
        if (px < 10) {
            px = 10;
        }

        ui::size ts = p.measure_text(glyph, px);
        int32_t tx = icon_box.x + (icon_box.w - ts.w) / 2;
        int32_t ty = icon_box.y + (icon_box.h - ts.h) / 2
                   + p.font_ascent(px);
        p.text({ tx, ty }, glyph, px,
               m_pin->label[0] ? m_accent : PIN_FALLBACK_FG);
    }

    void on_pointer_enter() override {
        m_hover = true;
        invalidate();

        if (on_hover) {
            on_hover(m_index, true);
        }
    }

    void on_pointer_leave() override {
        m_hover = false;
        m_pressed = false;
        invalidate();

        if (on_hover) {
            on_hover(m_index, false);
        }
    }

    bool on_pointer_down(const ui::pointer_event&) override {
        m_pressed = true;
        invalidate();

        return true;
    }

    bool on_pointer_up(const ui::pointer_event& e) override {
        bool inside = e.pos.x >= 0 && e.pos.y >= 0 &&
                      e.pos.x < m_frame.w && e.pos.y < m_frame.h;
        bool fire = m_pressed && inside;

        m_pressed = false;
        invalidate();

        if (fire && on_launch && m_pin->path[0]) {
            on_launch(*m_pin);
        }

        return true;
    }

private:
    stlxgfx_surface_t* m_icon = nullptr;
    const stlxconf_pin_t* m_pin = nullptr;
    int32_t m_index = 0;
    uint32_t m_accent = 0;
    int32_t m_icon_px = 0;
    bool m_hover = false;
    bool m_pressed = false;
};

} // namespace

/* A view over a run of backdrop rows, so a tree paints in place */
static stlxgfx_surface_t* row_view(stlxgfx_surface_t* backdrop,
                                   int32_t y, int32_t h) {
    return stlxgfx_surface_from_buffer(
        backdrop->pixels + static_cast<uint32_t>(y) * backdrop->pitch,
        backdrop->width, static_cast<uint32_t>(h), backdrop->pitch,
        32, 16, 8, 0);
}

/* A picture of `glass` at its size, or null when memory runs out */
static stlxgfx_surface_t* paint_glass(const stlxgfx_surface_t* frost, const material::surface& glass) {
    uint32_t width = static_cast<uint32_t>(glass.bounds.w);
    uint32_t height = static_cast<uint32_t>(glass.bounds.h);
    stlxgfx_surface_t* picture = stlxgfx_create_surface(width, height, 32, 16, 8, 0);
    if (!picture) {
        return nullptr;
    }

    material::draw(picture, glass.bounds.x, glass.bounds.y, frost, glass, glass.bounds);

    return picture;
}

static uint64_t monotonic_ns() {
    timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull
         + static_cast<uint64_t>(ts.tv_nsec);
}

/* The default interface summarized as a short bar string, and its name, empty without an interface */
static network_summary read_network() {
    struct stlx_net_status st;
    if (stlx_net_get_status(&st) != 0 || st.if_count == 0) {
        return { "no network", "" };
    }

    const struct stlx_ifinfo* def = stlx_net_default_if(&st);
    if (!def) {
        return { "no network", "" };
    }

    if (!(def->flags & STLX_IFF_UP)) {
        return { "disconnected", def->name };
    }
    if (!(def->flags & STLX_IFF_CONFIGURED)) {
        return { "unconfigured", def->name };
    }

    char address[32];
    uint32_t ip = def->ipv4_addr;
    snprintf(address, sizeof(address), "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);

    return { address, def->name };
}

/* `widget`'s frame in the coordinates of its tree's root */
static ui::rect frame_on_tree(const ui::widget* widget) {
    ui::rect frame = widget->frame();
    for (const ui::widget* up = widget->parent(); up; up = up->parent()) {
        frame.x += up->frame().x;
        frame.y += up->frame().y;
    }

    return frame;
}

/* The file name of a launcher's program, which by convention is the id the program gives when it connects */
static const char* launcher_app_id(const stlxconf_pin_t& pin) {
    const char* slash = strrchr(pin.path, '/');
    return slash ? slash + 1 : pin.path;
}

int dm_panels::init(
    uint32_t screen_w,
    uint32_t screen_h,
    const stlxconf_t& conf,
    stlxgfx_surface_t* backdrop,
    const stlxgfx_surface_t* frost,
    stats_sampler& stats
) {
    m_conf = &conf;
    m_stats = &stats;
    m_frost = frost;

    m_width = screen_w;
    m_height = screen_h;
    m_dock_h = static_cast<int32_t>(conf.taskbar_height);
    m_dock_y = static_cast<int32_t>(screen_h) - m_dock_h;

    m_band = row_view(backdrop, 0, BAR_H);
    m_dock = row_view(backdrop, m_dock_y, m_dock_h);
    if (!m_band || !m_dock) {
        return -1;
    }

    int32_t width = static_cast<int32_t>(screen_w);
    m_bar_glass = paint_glass(frost, { { 0, 0, width, BAR_H } });
    m_dock_glass = paint_glass(frost, { { 0, m_dock_y, width, m_dock_h }, 0, material::DOCK_RIM_FADE });
    if (!m_bar_glass || !m_dock_glass) {
        return -1;
    }

    /* The dock's accent top edge is inside its layout and is restored
     * after each repaint */
    stlxgfx_fill_rect(m_dock, 0, 0, screen_w, 1, conf.accent_color);

    m_tip_font = stlxgfx_font_open(STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_CAPTION_SIZE);
    if (!m_tip_font) {
        return -1;
    }
    stlxgfx_font_metrics_get(m_tip_font, &m_tip_fm);

    m_host.set_root(m_bar.build(m_bar_glass));
    m_bar.readouts()->show_stats(*m_stats);

    m_popover.set_output_width(static_cast<int32_t>(screen_w));
    m_menu = std::make_unique<system_menu>(conf, *m_stats);
    m_stats_panel = std::make_unique<stats_panel>(*m_stats, m_network);

    m_menu->on_spawn = [this](const char* path, const char* args) {
        m_popover.close();
        if (on_spawn) {
            on_spawn(path, args);
        }
    };

    m_menu->on_power = [this]() {
        m_popover.close_now();
        if (on_power) {
            on_power();
        }
    };

    m_stats_panel->on_spawn = [this](const char* path, const char* args) {
        if (!m_stats_panel->pinned()) {
            m_popover.close();
        }

        if (on_spawn) {
            on_spawn(path, args);
        }
    };

    m_bar.mark()->on_press = [this]() {
        toggle_popover(*m_menu);
    };

    m_bar.mark()->on_enter = [this]() {
        switch_popover(*m_menu);
    };

    m_bar.readouts()->on_press = [this]() {
        toggle_popover(*m_stats_panel);
    };

    m_bar.readouts()->on_enter = [this]() {
        switch_popover(*m_stats_panel);
    };

    m_network = read_network();
    m_net_query_ns = monotonic_ns();
    m_bar.readouts()->show_network(m_network.address.c_str());

    /* The dock: the config's pins centered between flex spacers */
    auto dock = std::make_unique<glass_row>(m_dock_glass);
    dock->s().align_items = ui::align::center;

    /* Tiles are one pixel wider than their icons on each side, so the
     * icon pitch matches the configured icon size plus spacing */
    int32_t spacing = static_cast<int32_t>(conf.taskbar_spacing);
    dock->s().gap = spacing >= 2 ? spacing - 2 : 0;

    ui::box* lead = dock->add<ui::box>();
    lead->s().main = ui::length::flex();

    int32_t icon_px = static_cast<int32_t>(conf.taskbar_icon_size);
    for (uint32_t i = 0; i < conf.pin_count; i++) {
        const stlxconf_pin_t& pin = conf.pins[i];

        stlxgfx_surface_t* icon = nullptr;
        if (pin.icon_path[0]) {
            icon = stlxgfx_load_image(pin.icon_path);
        }
        if (!icon) {
            icon = stlxgfx_load_image(STLXCONF_DEFAULT_ICON);
        }
        if (icon) {
            m_icons.push_back(icon);
        }

        uint32_t accent = PIN_ACCENTS[
            i % (sizeof(PIN_ACCENTS) / sizeof(PIN_ACCENTS[0]))];
        dock_button* btn = dock->add<dock_button>(
            icon, pin, static_cast<int32_t>(i), accent, icon_px);
        btn->s().main = ui::length::fixed(icon_px + 2);
        btn->s().cross = ui::length::fixed(icon_px + 2);
        btn->on_launch = [this](const stlxconf_pin_t& launched) {
            if (on_launch) {
                on_launch(launched);
            }
        };
        btn->on_hover = [this](int32_t index, bool entered) {
            hover_pin(index, entered);
        };
    }

    ui::box* tail = dock->add<ui::box>();
    tail->s().main = ui::length::flex();

    m_dock_host.set_root(std::move(dock));

    return 0;
}

/* The views keep their rows and only follow the pixels, so a rebind cannot fail */
void dm_panels::rebind(stlxgfx_surface_t* backdrop, const stlxgfx_surface_t* frost) {
    m_band->pixels = backdrop->pixels;
    m_band->pitch = backdrop->pitch;
    m_dock->pixels = backdrop->pixels + static_cast<uint32_t>(m_dock_y) * backdrop->pitch;
    m_dock->pitch = backdrop->pitch;
    m_frost = frost;
}

void dm_panels::shutdown() {
    m_popover.use_picture_maker(nullptr);

    /* Roots go first, dock buttons reference the icon surfaces */
    m_host.set_root(nullptr);
    m_dock_host.set_root(nullptr);
    m_bar = top_bar();
    m_hover_pin = -1;
    m_drawn_hover_pin = -1;

    for (stlxgfx_surface_t* icon : m_icons) {
        stlxgfx_destroy_surface(icon);
    }
    m_icons.clear();

    stlxgfx_destroy_surface(m_band);
    stlxgfx_destroy_surface(m_dock);
    stlxgfx_destroy_surface(m_bar_glass);
    m_band = nullptr;
    m_dock = nullptr;
    m_bar_glass = nullptr;

    stlxgfx_destroy_surface(m_dock_glass);
    m_dock_glass = nullptr;

    stlxgfx_font_close(m_tip_font);
    m_tip_font = nullptr;
}

void dm_panels::hover_pin(int32_t index, bool entered) {
    if (entered) {
        m_hover_pin = index;
        return;
    }

    if (m_hover_pin == index) {
        m_hover_pin = -1;
    }
}

/* Screen-space icon box of one pin, the flex-centered row reduced to
 * arithmetic: tiles are icon plus two, pitch adds the gap */
damage_list::rect dm_panels::pin_icon_rect(int32_t index) const {
    int32_t n = static_cast<int32_t>(m_conf->pin_count);
    int32_t icon = static_cast<int32_t>(m_conf->taskbar_icon_size);
    int32_t tile = icon + 2;
    int32_t spacing = static_cast<int32_t>(m_conf->taskbar_spacing);
    int32_t gap = spacing >= 2 ? spacing - 2 : 0;

    int32_t total = n * tile + (n - 1) * gap;
    int32_t start = (static_cast<int32_t>(m_width) - total) / 2;
    int32_t x = start + index * (tile + gap) + 1;
    int32_t y = m_dock_y + (m_dock_h - icon) / 2;

    return { x, y, icon, icon };
}

void dm_panels::flush(damage_list& backdrop_damage, damage_list& top_damage) {
    m_popover.flush(top_damage);
    sync_bar_targets();

    if (m_band && m_host.dirty()) {
        m_host.layout_now(static_cast<int32_t>(m_width), BAR_H);

        std::vector<ui::rect> out;
        m_host.paint_now(m_band, out);

        for (const ui::rect& r : out) {
            backdrop_damage.add(r.x, r.y, r.w, r.h);
        }
    }

    if (m_dock && m_dock_host.dirty()) {
        m_dock_host.layout_now(static_cast<int32_t>(m_width), m_dock_h);

        std::vector<ui::rect> out;
        m_dock_host.paint_now(m_dock, out);
        stlxgfx_fill_rect(m_dock, 0, 0, m_width, 1, m_conf->accent_color);

        /* Dock rects translate down to the band's screen position */
        for (const ui::rect& r : out) {
            backdrop_damage.add(r.x, r.y + m_dock_y, r.w, r.h);
        }
    }

    /* A tooltip appearing or vanishing repaints the dock strip plus
     * its reach above the band */
    if (m_hover_pin != m_drawn_hover_pin) {
        top_damage.add(0, m_dock_y - TIP_REACH,
                       static_cast<int32_t>(m_width), TIP_REACH + m_dock_h);
        m_drawn_hover_pin = m_hover_pin;
    }
}

/* A press on a target opens its panel or, when that panel is open, closes it */
void dm_panels::toggle_popover(popover_panel& panel) {
    if (m_popover.panel() == &panel) {
        m_popover.close();
        return;
    }

    m_popover.open(panel, m_frost);
}

/* While an unpinned panel is open, the pointer entering another panel's target opens that panel in its place */
void dm_panels::switch_popover(popover_panel& panel) {
    popover_panel* open = m_popover.panel();
    bool is_other_open = open && open != &panel;
    if (is_other_open && !open->pinned()) {
        m_popover.open(panel, m_frost);
    }
}

void dm_panels::sync_bar_targets() {
    popover_panel* open = m_popover.panel();
    m_bar.mark()->set_open(open == m_menu.get());
    m_bar.readouts()->set_open(open == m_stats_panel.get());
}

bool dm_panels::dismisses_popover(int32_t x, int32_t y) const {
    popover_panel* open = m_popover.panel();
    if (!open) {
        return false;
    }

    bool keeps_open = open->pinned() || m_popover.contains(x, y);
    if (keeps_open) {
        return false;
    }

    ui::point at = { x, y };
    bool on_target = frame_on_tree(m_bar.mark()).contains(at) || frame_on_tree(m_bar.readouts()).contains(at);

    return !on_target;
}

bool dm_panels::find_tooltip(damage_list::rect& tip, const char*& label) const {
    if (m_hover_pin < 0 ||
        m_hover_pin >= static_cast<int32_t>(m_conf->pin_count)) {
        return false;
    }

    const stlxconf_pin_t& pin = m_conf->pins[m_hover_pin];
    if (!pin.label[0]) {
        return false;
    }

    int32_t tw = stlxgfx_text_width(m_tip_font, pin.label,
                                    strlen(pin.label));
    int32_t th = m_tip_fm.ascent + m_tip_fm.descent;
    damage_list::rect icon = pin_icon_rect(m_hover_pin);

    int32_t tip_w = tw + 2 * TIP_PAD_H;
    int32_t tip_h = th + 2 * TIP_PAD_V;
    int32_t tip_x = icon.x + icon.w / 2 - tip_w / 2;
    int32_t tip_y = icon.y - tip_h - TIP_GAP;

    if (tip_x < 2) {
        tip_x = 2;
    }
    if (tip_x + tip_w > static_cast<int32_t>(m_width) - 2) {
        tip_x = static_cast<int32_t>(m_width) - tip_w - 2;
    }
    if (tip_y < 2) {
        tip_y = 2;
    }

    tip = { tip_x, tip_y, tip_w, tip_h };
    label = pin.label;

    return true;
}

damage_list::rect dm_panels::top_bounds() const {
    damage_list::rect tip;
    const char* label;
    if (!find_tooltip(tip, label)) {
        return {};
    }

    return tip;
}

void dm_panels::compose_top(stlxgfx_surface_t* back,
                            const damage_list::rect& r) {
    damage_list::rect tip;
    const char* label;
    if (!find_tooltip(tip, label)) {
        return;
    }

    if (tip.x >= r.x + r.w || tip.x + tip.w <= r.x ||
        tip.y >= r.y + r.h || tip.y + tip.h <= r.y) {
        return;
    }

    /* A view over the compose rect clips the box and text for free */
    stlxgfx_surface_t* view = stlxgfx_surface_from_buffer(
        back->pixels + static_cast<uint32_t>(r.y) * back->pitch
            + static_cast<uint32_t>(r.x) * 4,
        static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h),
        back->pitch, 32, 16, 8, 0);
    if (!view) {
        return;
    }

    view->flags = back->flags;

    stlxgfx_fill_rounded_rect(view, tip.x - r.x, tip.y - r.y,
                              static_cast<uint32_t>(tip.w),
                              static_cast<uint32_t>(tip.h),
                              static_cast<uint32_t>(TIP_RADIUS), TIP_BG);
    stlxgfx_draw_text(view, m_tip_font, tip.x - r.x + TIP_PAD_H,
                      tip.y - r.y + TIP_PAD_V + m_tip_fm.ascent,
                      label, strlen(label), TIP_FG);
    stlxgfx_destroy_surface(view);
}

void dm_panels::pointer_move(int32_t x, int32_t y) {
    if (m_popover.contains(x, y)) {
        m_host.pointer_move(-1, -1);
        m_dock_host.pointer_move(-1, -1);
        m_popover.pointer_move(x, y);
        return;
    }

    m_popover.pointer_move(-1, -1);
    if (y >= m_dock_y) {
        m_host.pointer_move(-1, -1);
        m_dock_host.pointer_move(x, y - m_dock_y);
        return;
    }

    m_dock_host.pointer_move(-1, -1);
    m_host.pointer_move(x, y);
}

void dm_panels::pointer_button(int32_t x, int32_t y, uint8_t btn,
                               bool down) {
    if (m_popover.contains(x, y)) {
        m_popover.pointer_button(x, y, btn, down);
        return;
    }

    if (y >= m_dock_y) {
        m_dock_host.pointer_button(x, y - m_dock_y, btn, down);
        return;
    }

    m_host.pointer_button(x, y, btn, down);
}

int64_t dm_panels::clock_timeout_ns(uint64_t now_ns) const {
    uint64_t into_period = now_ns % STATS_SAMPLE_PERIOD_NS;
    return static_cast<int64_t>(STATS_SAMPLE_PERIOD_NS - into_period);
}

void dm_panels::clock_tick() {
    if (!m_bar.readouts()) {
        return;
    }

    uint64_t now = monotonic_ns();
    if (m_stats->sample(now)) {
        m_bar.show_clock();
        m_bar.readouts()->show_stats(*m_stats);
        m_popover.refresh();
    }

    if (now - m_net_query_ns >= NET_POLL_NS) {
        m_net_query_ns = now;
        m_network = read_network();
        m_bar.readouts()->show_network(m_network.address.c_str());
    }
}

void dm_panels::show_focused_app(const char* app_id) {
    if (!app_id || !app_id[0]) {
        m_bar.show_app_name("");
        return;
    }

    for (uint32_t i = 0; i < m_conf->pin_count; i++) {
        const stlxconf_pin_t& pin = m_conf->pins[i];
        if (strcmp(launcher_app_id(pin), app_id) == 0 && pin.label[0]) {
            m_bar.show_app_name(pin.label);
            return;
        }
    }

    m_bar.show_app_name(app_id);
}
