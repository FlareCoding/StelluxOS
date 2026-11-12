/* The compositor's panels on its own toolkit hosts. The bar's and the dock's trees paint straight into the backdrop
 * rows they own, so compose needs no step for them, and the popover's tree paints into a picture above the windows. */
#include "panels.hpp"
#include "material.hpp"
#include "shell/programs.hpp"

#include <stlx/net.h>
#include <stlxtheme/theme.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

/* The tooltip is glass inside a brass hairline premixed over the warm glass tint, so the tooltip draws opaque */
constexpr uint32_t TIP_LINE = 0xFF3A3326;
constexpr uint32_t TIP_FG = STLXTHEME_TEXT;
constexpr int32_t TIP_RADIUS = STLXTHEME_CONTROL_RADIUS;
constexpr int32_t TIP_PAD_H = 8;
constexpr int32_t TIP_PAD_V = 4;
constexpr int32_t TIP_GAP = 8;

/* The tooltip pops above the dock, so hover transitions damage this
 * far beyond the band */
constexpr int32_t TIP_REACH = 48;

/* The tick runs on every wakeup, so it asks for the rarely changing network state only this often */
constexpr uint64_t NET_POLL_NS = 3000000000ull;

static_assert(STLXCONF_MAX_TASKBAR <= 64, "a launcher's running tick is one bit of a 64-bit mask");

/* A view over a run of backdrop rows, so a tree paints in place */
static stlxgfx_surface_t* row_view(stlxgfx_surface_t* backdrop,
                                   int32_t y, int32_t h) {
    return stlxgfx_surface_from_buffer(
        backdrop->pixels + static_cast<uint32_t>(y) * backdrop->pitch,
        backdrop->width, static_cast<uint32_t>(h), backdrop->pitch,
        32, 16, 8, 0);
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

    m_dock_layout = lay_out_dock(static_cast<int32_t>(screen_w), static_cast<int32_t>(screen_h), conf.pin_count);

    m_band = row_view(backdrop, 0, BAR_H);
    m_dock = row_view(backdrop, m_dock_layout.band_y, m_dock_layout.band_h);
    m_bar_glass = stlxgfx_create_surface(screen_w, BAR_H, 32, 16, 8, 0);
    if (!m_band || !m_dock || !m_bar_glass) {
        return -1;
    }

    material::surface bar_glass = { { 0, 0, static_cast<int32_t>(screen_w), BAR_H } };
    material::draw(m_bar_glass, 0, 0, frost, bar_glass, bar_glass.bounds);

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

    auto band = std::make_unique<dock_band>(backdrop, frost, m_dock_layout);
    if (!band->picture()) {
        return -1;
    }

    for (uint32_t i = 0; i < conf.pin_count; i++) {
        stlxgfx_surface_t* icon = load_launcher_icon(conf.pins[i]);
        m_icons.push_back(icon);

        dock_tile* tile = band->add<dock_tile>(icon, conf.pins[i], i);
        tile->on_launch = [this](const stlxconf_pin_t& pin) {
            launch(pin);
        };

        tile->on_hover = [this](uint32_t index, bool entered) {
            hover_pin(static_cast<int32_t>(index), entered);
        };

        m_tiles.push_back(tile);
    }

    m_dock_host.set_root(std::move(band));

    m_quick = std::make_unique<quick_actions>(conf, m_icons);
    m_quick->on_launch = [this](const stlxconf_pin_t& pin) {
        m_popover.close();
        launch(pin);
    };

    m_quick->on_run = [this](const stlxconf_action_t& action) {
        m_popover.close();
        run_action(action);
    };

    m_quick->on_run_program = [this](const char* program, const char* args) {
        m_popover.close();
        run_in_terminal(program, args);
    };

    m_quick->on_copy = [this](const std::string& text) {
        m_popover.close();
        if (on_copy) {
            on_copy(text);
        }
    };

    m_quick->on_copy_file = [this](const std::string& path) {
        m_popover.close();
        if (on_copy_file) {
            on_copy_file(path);
        }
    };

    m_quick->on_exec = [this](const char* program, const std::vector<std::string>& args) {
        m_popover.close();
        return on_exec && on_exec(program, args);
    };

    m_quick->on_clipboard_paste = [this](uint64_t id) {
        m_popover.close();
        if (on_clipboard_paste) {
            on_clipboard_paste(id);
        }
    };

    m_quick->on_clipboard_copy = [this](uint64_t id) {
        m_popover.close();
        if (on_clipboard_copy) {
            on_clipboard_copy(id);
        }
    };

    m_quick->on_clipboard_pin = [this](uint64_t id, bool pinned) {
        if (on_clipboard_pin) {
            on_clipboard_pin(id, pinned);
        }
    };

    m_quick->on_clipboard_remove = [this](uint64_t id) {
        if (on_clipboard_remove) {
            on_clipboard_remove(id);
        }
    };

    m_quick->on_clipboard_clear = [this]() {
        if (on_clipboard_clear) {
            on_clipboard_clear();
        }
    };

    return 0;
}

void dm_panels::toggle_quick_actions() {
    m_quick->open_in_clipboard(false);
    toggle_popover(*m_quick);
}

void dm_panels::toggle_clipboard() {
    if (m_popover.panel() != m_quick.get()) {
        m_quick->open_in_clipboard(true);
        m_popover.open(*m_quick, m_frost);
    } else if (m_quick->shows_clipboard()) {
        m_popover.close();
    } else {
        m_quick->show_clipboard();
    }
}

/* The views keep their rows and only follow the pixels, so a rebind cannot fail */
void dm_panels::rebind(stlxgfx_surface_t* backdrop, const stlxgfx_surface_t* frost) {
    m_band->pixels = backdrop->pixels;
    m_band->pitch = backdrop->pitch;
    m_dock->pixels = backdrop->pixels + static_cast<uint32_t>(m_dock_layout.band_y) * backdrop->pitch;
    m_dock->pitch = backdrop->pitch;
    m_frost = frost;
}

void dm_panels::shutdown() {
    m_popover.use_picture_maker(nullptr);

    /* Roots go first, since the dock's tiles reference the icon surfaces */
    m_host.set_root(nullptr);
    m_dock_host.set_root(nullptr);
    m_tiles.clear();
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

void dm_panels::launch(const stlxconf_pin_t& pin) {
    if (on_unminimize && on_unminimize(pin)) {
        return;
    }

    if (on_launch) {
        on_launch(pin);
    }
}

void dm_panels::run_in_terminal(const char* program, const char* args) {
    std::string command = args[0] ? std::string(program) + " " + args : program;
    if (on_spawn) {
        on_spawn(TERMINAL_PATH, command.c_str());
    }
}

void dm_panels::run_action(const stlxconf_action_t& action) {
    stlxconf_action_kind_t kind = stlxconf_action_kind(action.action);
    if (kind == STLXCONF_ACTION_EXEC && on_spawn) {
        on_spawn(action.path, action.args);
    } else if (kind == STLXCONF_ACTION_POWER && on_power) {
        on_power();
    }
}

damage_list::rect dm_panels::launcher_tile(uint32_t index) const {
    if (index >= m_conf->pin_count) {
        return {};
    }

    return dock_tile_rect(m_dock_layout, index);
}

int32_t dm_panels::launcher_of(const char* app_id) const {
    if (!app_id[0]) {
        return -1;
    }

    for (uint32_t i = 0; i < m_conf->pin_count; i++) {
        if (launches_app(m_conf->pins[i], app_id)) {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

int32_t dm_panels::launcher_at(int32_t x, int32_t y) const {
    for (uint32_t i = 0; i < m_conf->pin_count; i++) {
        damage_list::rect tile = launcher_tile(i);
        if (x >= tile.x && x < tile.x + tile.w && y >= tile.y && y < tile.y + tile.h) {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

void dm_panels::light_launcher(int32_t index, bool lit) {
    if (index >= 0 && static_cast<size_t>(index) < m_tiles.size()) {
        m_tiles[static_cast<size_t>(index)]->show_drop_hover(lit);
    }
}

void dm_panels::show_running_launchers(uint64_t running, int32_t focused) {
    m_quick->show_running(running);
    for (size_t i = 0; i < m_tiles.size(); i++) {
        bool is_running = ((running >> i) & 1) != 0;
        m_tiles[i]->show_running(is_running, is_running && static_cast<int32_t>(i) == focused);
    }
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
        m_dock_host.layout_now(static_cast<int32_t>(m_width), m_dock_layout.band_h);

        std::vector<ui::rect> out;
        m_dock_host.paint_now(m_dock, out);

        /* Dock rects translate down to the band's screen position */
        for (const ui::rect& r : out) {
            backdrop_damage.add(r.x, r.y + m_dock_layout.band_y, r.w, r.h);
        }
    }

    /* A tooltip appearing or vanishing repaints the dock strip plus
     * its reach above the band */
    if (m_hover_pin != m_drawn_hover_pin) {
        top_damage.add(0, m_dock_layout.band_y - TIP_REACH,
                       static_cast<int32_t>(m_width), TIP_REACH + m_dock_layout.band_h);
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

/* While an unpinned panel of a bar target is open, the pointer entering another target opens that one's panel in
 * its place */
void dm_panels::switch_popover(popover_panel& panel) {
    popover_panel* open = m_popover.panel();
    bool is_bar_panel_open = open == m_menu.get() || open == m_stats_panel.get();
    if (is_bar_panel_open && open != &panel && !open->pinned()) {
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
    damage_list::rect tile = dock_tile_rect(m_dock_layout, static_cast<uint32_t>(m_hover_pin));

    int32_t tip_w = tw + 2 * TIP_PAD_H;
    int32_t tip_h = th + 2 * TIP_PAD_V;
    int32_t tip_x = tile.x + tile.w / 2 - tip_w / 2;
    int32_t tip_y = m_dock_layout.shelf.y - tip_h - TIP_GAP;

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
                              static_cast<uint32_t>(TIP_RADIUS), TIP_LINE);
    material::surface glass = { { tip.x + 1, tip.y + 1, tip.w - 2, tip.h - 2 }, TIP_RADIUS - 1 };
    material::draw(view, r.x, r.y, m_frost, glass, r);
    stlxgfx_draw_text(view, m_tip_font, tip.x - r.x + TIP_PAD_H,
                      tip.y - r.y + TIP_PAD_V + m_tip_fm.ascent,
                      label, strlen(label), TIP_FG);
    stlxgfx_destroy_surface(view);
}

void dm_panels::pointer_move(int32_t x, int32_t y) {
    bool is_dock_held = m_dock_host.holds_pointer();
    if (!is_dock_held && m_popover.contains(x, y)) {
        m_host.pointer_move(-1, -1);
        m_dock_host.pointer_move(-1, -1);
        m_popover.pointer_move(x, y);
        return;
    }

    m_popover.pointer_move(-1, -1);
    if (is_dock_held || y >= m_dock_layout.band_y) {
        m_host.pointer_move(-1, -1);
        m_dock_host.pointer_move(x, y - m_dock_layout.band_y);
        return;
    }

    m_dock_host.pointer_move(-1, -1);
    m_host.pointer_move(x, y);
}

void dm_panels::pointer_button(int32_t x, int32_t y, uint8_t btn,
                               bool down) {
    bool is_dock_held = m_dock_host.holds_pointer();
    if (!is_dock_held && m_popover.contains(x, y)) {
        m_popover.pointer_button(x, y, btn, down);
        return;
    }

    if (is_dock_held || y >= m_dock_layout.band_y) {
        m_dock_host.pointer_button(x, y - m_dock_layout.band_y, btn, down);
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

/* Clipboard mode names the same app as where Enter pastes */
void dm_panels::show_focused_app(const char* app_id) {
    std::string label = app_id ? app_id : "";
    for (uint32_t i = 0; app_id && i < m_conf->pin_count; i++) {
        const stlxconf_pin_t& pin = m_conf->pins[i];
        if (launches_app(pin, app_id) && pin.label[0]) {
            label = pin.label;
            break;
        }
    }

    m_bar.show_app_name(label);
    m_quick->show_focused_app(label);
}
