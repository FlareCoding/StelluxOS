#include "system_menu.hpp"

#include "bar.hpp"
#include "chord.hpp"
#include "draw.hpp"

#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <sys/utsname.h>
#include <vector>

constexpr int32_t MENU_WIDTH = 272;
constexpr int32_t MENU_LEFT = top_bar::TARGET_INSET;
constexpr int32_t MENU_PADDING = 4;

constexpr int32_t HEADER_PADDING_TOP = 8;
constexpr int32_t HEADER_PADDING_BOTTOM = 9;
constexpr int32_t HEADER_PADDING_X = 10;
constexpr int32_t HEADER_GAP = 10;

constexpr int32_t DIVIDER_MARGIN_Y = 4;
constexpr int32_t DIVIDER_INSET = 6;

constexpr int32_t ROW_HEIGHT = 28;
constexpr int32_t ROW_PADDING_LEFT = 10;
constexpr int32_t ROW_PADDING_RIGHT = 6;

constexpr int32_t KEYCAP_HEIGHT = 18;
constexpr int32_t KEYCAP_PADDING_X = 5;
constexpr int32_t KEYCAP_GAP = 3;
constexpr float KEYCAP_TEXT_SIZE = 11.0f;

/* A menu row's label and the program it spawns, by its path and its space separated args */
struct menu_program {
    const char* label;
    const char* path;
    const char* args;
};

static const menu_program MENU_PROGRAMS[] = {
    { "Settings", "/bin/stlxsettings", "" },
    { "Task monitor", "/bin/stlxterm", "/bin/stlxtop" },
};

/* Leads to the power overlay, which asks whether to restart or shut down */
static const char* const POWER_LABEL = "Power...";

namespace {

/* A picture the widget owns, at its size */
class picture_view : public ui::widget {
public:
    explicit picture_view(stlxgfx_surface_t* picture) : m_picture(picture) {}
    ~picture_view() override { stlxgfx_destroy_surface(m_picture); }

    ui::size measure(ui::size) override {
        if (!m_picture) {
            return {};
        }

        return { static_cast<int32_t>(m_picture->width), static_cast<int32_t>(m_picture->height) };
    }

    void paint(ui::painter& p) override {
        p.image({ 0, 0 }, m_picture);
    }

private:
    stlxgfx_surface_t* m_picture;
};

class menu_divider : public ui::widget {
public:
    ui::size measure(ui::size avail) override {
        return { avail.w, 2 * DIVIDER_MARGIN_Y + 1 };
    }

    void paint(ui::painter& p) override {
        p.fill({ DIVIDER_INSET, DIVIDER_MARGIN_Y, m_frame.w - 2 * DIVIDER_INSET, 1 }, STLXTHEME_LINE_SUBTLE);
    }
};

/* One key of a shortcut, as a chip with the key's name */
class keycap {
public:
    explicit keycap(const std::string& name)
        : m_text(name, STLXTHEME_MONO_FONT_PATH, KEYCAP_TEXT_SIZE, STLXTHEME_TEXT_DIM),
          m_width(std::max(KEYCAP_HEIGHT, m_text.width() + 2 * KEYCAP_PADDING_X)),
          m_ring(make_ring_picture(m_width, KEYCAP_HEIGHT, STLXTHEME_CHIP_RADIUS, STLXTHEME_LINE)),
          m_hover_ring(make_ring_picture(m_width, KEYCAP_HEIGHT, STLXTHEME_CHIP_RADIUS, STLXTHEME_LINE_STRONG)) {}

    ~keycap() {
        stlxgfx_destroy_surface(m_ring);
        stlxgfx_destroy_surface(m_hover_ring);
    }

    keycap(const keycap&) = delete;
    keycap& operator=(const keycap&) = delete;

    int32_t width() const { return m_width; }

    void set_hover(bool hover);
    void paint(ui::painter& p, int32_t x, int32_t top) const;

private:
    text_picture       m_text;
    int32_t            m_width;
    stlxgfx_surface_t* m_ring;
    stlxgfx_surface_t* m_hover_ring;
    bool               m_hover = false;
};

/* An item, chosen by a release over it wherever the press began, so the pointer can press the mark and
 * release over an item */
class menu_row : public ui::widget {
public:
    menu_row(const char* label, const std::vector<std::string>& keys)
        : m_label(label, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT) {
        for (const std::string& key : keys) {
            m_keycaps.push_back(std::make_unique<keycap>(key));
        }
    }

    std::function<void()> on_choose;

    ui::size measure(ui::size avail) override {
        return { avail.w, ROW_HEIGHT };
    }

    void paint(ui::painter& p) override;

    void on_pointer_enter() override {
        set_hover(true);
    }

    void on_pointer_leave() override {
        set_hover(false);
    }

    bool on_pointer_up(const ui::pointer_event&) override {
        if (on_choose) {
            on_choose();
        }

        return true;
    }

private:
    void set_hover(bool hover);

    text_picture                         m_label;
    std::vector<std::unique_ptr<keycap>> m_keycaps;
    bool                                 m_hover = false;
};

} // namespace

void keycap::set_hover(bool hover) {
    m_hover = hover;
    m_text.set_color(hover ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT_DIM);
}

void keycap::paint(ui::painter& p, int32_t x, int32_t top) const {
    p.rounded_rect({ x, top, m_width, KEYCAP_HEIGHT }, STLXTHEME_CHIP_RADIUS, STLXTHEME_SURFACE);
    p.image({ x, top }, m_hover ? m_hover_ring : m_ring);
    m_text.draw(p, x + (m_width - m_text.width()) / 2, top, KEYCAP_HEIGHT);
}

void menu_row::paint(ui::painter& p) {
    if (m_hover) {
        p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_SELECTED);
    }

    m_label.draw(p, ROW_PADDING_LEFT, 0, m_frame.h);

    int32_t x = m_frame.w - ROW_PADDING_RIGHT;
    int32_t top = (m_frame.h - KEYCAP_HEIGHT) / 2;
    for (size_t i = m_keycaps.size(); i-- > 0;) {
        const keycap& key = *m_keycaps[i];
        x -= key.width();
        key.paint(p, x, top);
        x -= KEYCAP_GAP;
    }
}

void menu_row::set_hover(bool hover) {
    m_hover = hover;
    for (const std::unique_ptr<keycap>& key : m_keycaps) {
        key->set_hover(hover);
    }

    invalidate();
}

/* The release up to its first dash, such as 3.0 */
static std::string release_name() {
    struct utsname names;
    if (uname(&names) != 0) {
        return "";
    }

    std::string release = names.release;

    return release.substr(0, release.find('-'));
}

/* Empty while the sampler has read no uptime */
static std::string uptime_text(const stats_sampler& stats) {
    if (stats.uptime_ns() == 0) {
        return "";
    }

    return "up " + format_uptime(stats.uptime_ns());
}

/* The keys of the first live hotkey that runs the same program, none when no hotkey does. A hotkey is live
 * when it is an exec shortcut with a path and a chord that parses, as the server binds them. */
static std::vector<std::string> shortcut_keys(const stlxconf_t& conf, const menu_program& program) {
    for (uint32_t i = 0; i < conf.shortcut_count; i++) {
        const stlxconf_shortcut_t& shortcut = conf.shortcuts[i];
        bool runs_program = strcmp(shortcut.action, "exec") == 0;
        bool same_path = strcmp(shortcut.path, program.path) == 0;
        bool same_args = strcmp(shortcut.args, program.args) == 0;
        if (!runs_program || !same_path || !same_args) {
            continue;
        }

        std::vector<std::string> keys = chord_keycaps(shortcut.key);
        if (!keys.empty()) {
            return keys;
        }
    }

    return {};
}

int32_t system_menu::width() const {
    return MENU_WIDTH;
}

int32_t system_menu::left() const {
    return MENU_LEFT;
}

std::unique_ptr<ui::widget> system_menu::build() {
    auto root = std::make_unique<ui::box>(ui::axis::column);
    root->s().padding = ui::edge_insets::all(MENU_PADDING);

    ui::box* header = root->add<ui::box>(ui::axis::row);
    header->s().align_items = ui::align::center;
    header->s().gap = HEADER_GAP;
    header->s().padding = { HEADER_PADDING_TOP, HEADER_PADDING_X, HEADER_PADDING_BOTTOM, HEADER_PADDING_X };

    header->add<picture_view>(make_mark_picture(STLXTHEME_TEXT));
    ui::label* title = header->add<ui::label>("Stellux " + release_name());
    title->set_weight(ui::weight::semibold);
    title->s().main = ui::length::flex();
    m_uptime = header->add<mono_label>(uptime_text(m_stats), STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM);

    root->add<menu_divider>();
    for (const menu_program& program : MENU_PROGRAMS) {
        menu_row* row = root->add<menu_row>(program.label, shortcut_keys(m_conf, program));
        row->on_choose = [this, &program]() {
            on_spawn(program.path, program.args);
        };
    }

    root->add<menu_divider>();
    menu_row* power = root->add<menu_row>(POWER_LABEL, std::vector<std::string>());
    power->on_choose = [this]() {
        on_power();
    };

    return root;
}

void system_menu::refresh() {
    if (m_uptime) {
        m_uptime->set_text(uptime_text(m_stats));
    }
}

void system_menu::closed() {
    m_uptime = nullptr;
}
