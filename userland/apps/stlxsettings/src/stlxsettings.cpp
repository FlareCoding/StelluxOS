/* stlxsettings: the desktop configuration editor. A navigation rail
 * selects a page of grouped cards, edits land in the config struct
 * as they happen, and saving writes the file back and signals the
 * display manager to re-read it live.
 */
#include <stlxconf/conf.h>
#include <stlxui/stlxui.h>
#include <stlxwin/stlxwin.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

constexpr const char* DM_PID_PATH = "/tmp/stlxdm.pid";

/* App palette on top of the theme: a darker rail and raised cards */
constexpr uint32_t RAIL_BG = 0xFF181825;
constexpr uint32_t RAIL_HOVER = 0xFF232334;
constexpr uint32_t CARD_BG = 0xFF252536;

constexpr int32_t RAIL_W = 172;
constexpr int32_t NAV_H = 32;
constexpr int32_t CARD_RADIUS = 10;
constexpr int32_t ROW_LABEL_W = 150;
constexpr int32_t SWATCH_PX = 28;
constexpr uint32_t TITLE_PX = 19;
constexpr uint32_t CARD_TITLE_PX = 12;
constexpr uint32_t SMALL_PX = 12;

constexpr uint32_t PAGE_COUNT = 4;

static const char* const PAGE_NAMES[PAGE_COUNT] = {
    "Appearance", "Dock", "Input", "Startup"
};
static const char* const PAGE_BLURBS[PAGE_COUNT] = {
    "Wallpaper and the desktop's colors",
    "Pinned launchers and dock geometry",
    "Keyboard repeat behavior",
    "Programs and shortcuts at session start",
};

namespace {

/* One nav rail entry: a rounded row with an accent bar when selected */
class nav_item : public ui::widget {
public:
    explicit nav_item(std::string text) : m_text(std::move(text)) {}

    std::function<void()> on_select;

    void set_selected(bool v) {
        if (m_selected == v) {
            return;
        }

        m_selected = v;
        invalidate();
    }

    ui::size measure(ui::size) override {
        return { RAIL_W - 24, NAV_H };
    }

    void paint(ui::painter& p) override {
        const ui::theme& t = ui::theme::active();

        if (m_selected) {
            p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, 6, t.surface);
            p.rounded_rect({ 0, 8, 3, m_frame.h - 16 }, 2, t.accent);
        } else if (m_hover) {
            p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, 6, RAIL_HOVER);
        }

        ui::size text = p.measure_text(m_text, 0);
        int32_t baseline = (m_frame.h - text.h) / 2 + p.font_ascent(0);
        p.text({ 14, baseline }, m_text, 0,
               m_selected ? t.text : t.text_dim);
    }

    void on_pointer_enter() override {
        m_hover = true;
        invalidate();
    }

    void on_pointer_leave() override {
        m_hover = false;
        invalidate();
    }

    bool on_pointer_down(const ui::pointer_event&) override {
        return true;
    }

    bool on_pointer_up(const ui::pointer_event& e) override {
        bool inside = e.pos.x >= 0 && e.pos.y >= 0 &&
                      e.pos.x < m_frame.w && e.pos.y < m_frame.h;
        if (inside && on_select) {
            on_select();
        }

        return true;
    }

private:
    std::string m_text;
    bool m_selected = false;
    bool m_hover = false;
};

/* A raised rounded panel grouping related settings */
class card : public ui::box {
public:
    card() : ui::box(ui::axis::column) {
        m_style.padding = ui::edge_insets::all(16);
        m_style.gap = 10;
        m_style.main = ui::length::content();
    }

    void paint(ui::painter& p) override {
        p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, CARD_RADIUS,
                       CARD_BG);
    }
};

/* Everything the pages edit, shared by the builders and the footer */
struct app_state {
    stlxconf_t conf;
    ui::window_host* win = nullptr;
    ui::box* page_slot = nullptr;
    ui::box* page = nullptr;
    ui::label* page_title = nullptr;
    ui::label* page_blurb = nullptr;
    ui::label* status = nullptr;
    nav_item* nav[PAGE_COUNT] = {};
    uint32_t active_page = 0;
    bool dirty = false;
};

} // namespace

static app_state g_st;

static void set_status(const char* text) {
    if (g_st.status) {
        g_st.status->set_text(text);
    }
}

static void mark_dirty() {
    if (!g_st.dirty) {
        g_st.dirty = true;
        set_status("unsaved changes");
    }
}

static void format_hex(char* out, size_t cap, uint32_t value) {
    snprintf(out, cap, "0x%08X", value);
}

/* Bounded copy for the conf's fixed char arrays */
static void put_str(char* dst, size_t cap, const std::string& src) {
    size_t len = src.size() < cap - 1 ? src.size() : cap - 1;
    memcpy(dst, src.data(), len);
    dst[len] = '\0';
}

static uint32_t parse_u32(const std::string& s, int base) {
    return static_cast<uint32_t>(strtoul(s.c_str(), nullptr, base));
}

/* A labeled row inside a card: fixed label column, control after it */
static ui::box* make_row(ui::box* parent, const char* label) {
    ui::box* row = parent->add<ui::box>(ui::axis::row);
    row->s().main = ui::length::content();
    row->s().align_items = ui::align::center;
    row->s().gap = 12;

    ui::label* name = row->add<ui::label>(label);
    name->s().main = ui::length::fixed(ROW_LABEL_W);
    name->s().padding.left = 4;

    return row;
}

/* A bare row for list entries, no label column */
static ui::box* make_list_row(ui::box* parent) {
    ui::box* row = parent->add<ui::box>(ui::axis::row);
    row->s().main = ui::length::content();
    row->s().align_items = ui::align::center;
    row->s().gap = 8;

    return row;
}

/* One dim column caption inside a list header row */
static void add_caption(ui::box* header, const char* text,
                        ui::length main) {
    ui::label* cap = header->add<ui::label>(text);
    cap->set_font_size(CARD_TITLE_PX);
    cap->set_color(ui::theme::active().text_dim);
    cap->s().main = main;
    cap->s().padding.left = 4;
}

static card* make_card(ui::box* parent, const char* title) {
    card* c = parent->add<card>();

    ui::label* head = c->add<ui::label>(title);
    head->set_font_size(CARD_TITLE_PX);
    head->set_color(ui::theme::active().text_dim);
    head->s().main = ui::length::content();
    head->s().padding.left = 4;

    return c;
}

/* A color field: hex input plus a live swatch previewing the value */
static void make_color_row(ui::box* parent, const char* label,
                           uint32_t* value) {
    ui::box* row = make_row(parent, label);

    char text[16];
    format_hex(text, sizeof(text), *value);
    ui::text_input* field = row->add<ui::text_input>();
    field->s().main = ui::length::fixed(130);
    field->set_text(text);

    ui::canvas* swatch = row->add<ui::canvas>();
    swatch->s().main = ui::length::fixed(SWATCH_PX);
    swatch->s().cross = ui::length::fixed(SWATCH_PX);
    swatch->on_paint = [value](ui::painter& p, ui::size sz) {
        p.rounded_rect({ 0, 0, sz.w, sz.h }, 6,
                       ui::theme::active().surface_hover);
        p.rounded_rect({ 1, 1, sz.w - 2, sz.h - 2 }, 6,
                       0xFF000000u | *value);
    };

    field->on_change = [value, swatch](const std::string& s) {
        *value = parse_u32(s, 16);
        swatch->damage({});
        mark_dirty();
    };
}

/* A short numeric field writing through to a config integer */
static void make_number_row(ui::box* parent, const char* label,
                            uint32_t* value) {
    ui::box* row = make_row(parent, label);

    char text[16];
    snprintf(text, sizeof(text), "%u", *value);
    ui::text_input* field = row->add<ui::text_input>();
    field->s().main = ui::length::fixed(80);
    field->set_text(text);
    field->on_change = [value](const std::string& s) {
        *value = parse_u32(s, 10);
        mark_dirty();
    };
}

/* A full width path or text field writing through to a char array */
static void make_text_row(ui::box* parent, const char* label, char* dst,
                          size_t cap) {
    ui::box* row = make_row(parent, label);

    ui::text_input* field = row->add<ui::text_input>();
    field->s().main = ui::length::flex();
    field->set_text(dst);
    field->on_change = [dst, cap](const std::string& s) {
        put_str(dst, cap, s);
        mark_dirty();
    };
}

static void build_page(uint32_t index);

/* A "..." button opening the row actions for one list entry. The
 * mutation runs after the menu retires, then the page rebuilds. */
template <typename move_fn, typename remove_fn>
static void make_row_menu(ui::box* row, uint32_t index, uint32_t count,
                          move_fn mover, remove_fn remover) {
    ui::button* more = row->add<ui::button>("...");
    more->s().main = ui::length::fixed(36);
    more->on_click = [more, index, count, mover, remover]() {
        std::vector<ui::menu_item> items;
        items.push_back({ "Move up", [index, mover]() {
            mover(index, index - 1);
            build_page(g_st.active_page);
            mark_dirty();
        }, index > 0, false });
        items.push_back({ "Move down", [index, mover]() {
            mover(index, index + 1);
            build_page(g_st.active_page);
            mark_dirty();
        }, index + 1 < count, true });
        items.push_back({ "Remove", [index, remover]() {
            remover(index);
            build_page(g_st.active_page);
            mark_dirty();
        }, true, false });

        ui::menu::open_at(more, std::move(items));
    };
}

static void build_appearance(ui::box* page) {
    card* wall = make_card(page, "WALLPAPER");
    make_text_row(wall, "Image path", g_st.conf.wallpaper,
                  sizeof(g_st.conf.wallpaper));
    make_color_row(wall, "Fallback color", &g_st.conf.bg_color);

    card* colors = make_card(page, "BAR COLORS");
    make_color_row(colors, "Bar", &g_st.conf.bar_color);
    make_color_row(colors, "Accent", &g_st.conf.accent_color);
    make_color_row(colors, "Text", &g_st.conf.text_color);
    make_number_row(colors, "Bar font size", &g_st.conf.bar_font_size);
}

static void build_dock(ui::box* page) {
    card* geo = make_card(page, "GEOMETRY");
    make_number_row(geo, "Height", &g_st.conf.taskbar_height);
    make_number_row(geo, "Icon size", &g_st.conf.taskbar_icon_size);
    make_number_row(geo, "Spacing", &g_st.conf.taskbar_spacing);

    card* pins = make_card(page, "PINNED LAUNCHERS");

    auto move_pin = [](uint32_t from, uint32_t to) {
        stlxconf_pin_t tmp = g_st.conf.pins[from];
        g_st.conf.pins[from] = g_st.conf.pins[to];
        g_st.conf.pins[to] = tmp;
    };
    auto remove_pin = [](uint32_t at) {
        for (uint32_t i = at; i + 1 < g_st.conf.pin_count; i++) {
            g_st.conf.pins[i] = g_st.conf.pins[i + 1];
        }
        g_st.conf.pin_count--;
    };

    ui::box* header = make_list_row(pins);
    add_caption(header, "LABEL", ui::length::fixed(110));
    add_caption(header, "PROGRAM", ui::length::flex());
    add_caption(header, "ICON", ui::length::flex());
    header->add<ui::box>()->s().main = ui::length::fixed(36);

    for (uint32_t i = 0; i < g_st.conf.pin_count; i++) {
        stlxconf_pin_t* pin = &g_st.conf.pins[i];

        ui::box* row = make_list_row(pins);
        ui::text_input* label = row->add<ui::text_input>();
        label->s().main = ui::length::fixed(110);
        label->set_text(pin->label);
        label->on_change = [pin](const std::string& s) {
            put_str(pin->label, sizeof(pin->label), s);
            mark_dirty();
        };

        ui::text_input* path = row->add<ui::text_input>();
        path->s().main = ui::length::flex();
        path->set_text(pin->path);
        path->on_change = [pin](const std::string& s) {
            put_str(pin->path, sizeof(pin->path), s);
            mark_dirty();
        };

        ui::text_input* icon = row->add<ui::text_input>();
        icon->s().main = ui::length::flex();
        icon->set_text(pin->icon_path);
        icon->on_change = [pin](const std::string& s) {
            put_str(pin->icon_path, sizeof(pin->icon_path), s);
            mark_dirty();
        };

        make_row_menu(row, i, g_st.conf.pin_count, move_pin, remove_pin);
    }

    ui::button* add = pins->add<ui::button>("Add launcher");
    add->s().main = ui::length::content();
    add->s().align_self = ui::align::start;
    add->on_click = []() {
        if (g_st.conf.pin_count >= STLXCONF_MAX_TASKBAR) {
            set_status("launcher table is full");
            return;
        }

        stlxconf_pin_t* pin = &g_st.conf.pins[g_st.conf.pin_count];
        memset(pin, 0, sizeof(*pin));
        snprintf(pin->name, sizeof(pin->name), "pin%u",
                 g_st.conf.pin_count);
        put_str(pin->label, sizeof(pin->label), "New app");
        g_st.conf.pin_count++;

        build_page(g_st.active_page);
        mark_dirty();
    };
}

static void build_input(ui::box* page) {
    card* repeat = make_card(page, "KEYBOARD REPEAT");

    ui::checkbox* enabled = repeat->add<ui::checkbox>(
        "Repeat held keys", g_st.conf.key_repeat_delay_ms != 0);
    enabled->s().main = ui::length::content();
    enabled->on_change = [](bool on) {
        g_st.conf.key_repeat_delay_ms = on ? 400 : 0;
        build_page(g_st.active_page);
        mark_dirty();
    };

    if (g_st.conf.key_repeat_delay_ms != 0) {
        make_number_row(repeat, "Delay (ms)",
                        &g_st.conf.key_repeat_delay_ms);
        make_number_row(repeat, "Interval (ms)",
                        &g_st.conf.key_repeat_interval_ms);
    }
}

static void build_startup(ui::box* page) {
    card* autos = make_card(page, "AUTOSTART");

    auto move_auto = [](uint32_t from, uint32_t to) {
        stlxconf_autostart_t tmp = g_st.conf.autostart[from];
        g_st.conf.autostart[from] = g_st.conf.autostart[to];
        g_st.conf.autostart[to] = tmp;
    };
    auto remove_auto = [](uint32_t at) {
        for (uint32_t i = at; i + 1 < g_st.conf.autostart_count; i++) {
            g_st.conf.autostart[i] = g_st.conf.autostart[i + 1];
        }
        g_st.conf.autostart_count--;
    };

    ui::box* header = make_list_row(autos);
    add_caption(header, "PROGRAM", ui::length::flex());
    add_caption(header, "ARGUMENTS", ui::length::fixed(130));
    header->add<ui::box>()->s().main = ui::length::fixed(36);

    for (uint32_t i = 0; i < g_st.conf.autostart_count; i++) {
        stlxconf_autostart_t* as = &g_st.conf.autostart[i];

        ui::box* row = make_list_row(autos);
        ui::text_input* path = row->add<ui::text_input>();
        path->s().main = ui::length::flex();
        path->set_text(as->path);
        path->on_change = [as](const std::string& s) {
            put_str(as->path, sizeof(as->path), s);
            mark_dirty();
        };

        ui::text_input* args = row->add<ui::text_input>();
        args->s().main = ui::length::fixed(130);
        args->set_text(as->args);
        args->on_change = [as](const std::string& s) {
            put_str(as->args, sizeof(as->args), s);
            mark_dirty();
        };

        make_row_menu(row, i, g_st.conf.autostart_count, move_auto,
                      remove_auto);
    }

    ui::button* add = autos->add<ui::button>("Add program");
    add->s().main = ui::length::content();
    add->s().align_self = ui::align::start;
    add->on_click = []() {
        if (g_st.conf.autostart_count >= STLXCONF_MAX_AUTOSTART) {
            set_status("autostart table is full");
            return;
        }

        stlxconf_autostart_t* as =
            &g_st.conf.autostart[g_st.conf.autostart_count];
        memset(as, 0, sizeof(*as));
        snprintf(as->name, sizeof(as->name), "entry%u",
                 g_st.conf.autostart_count);
        g_st.conf.autostart_count++;

        build_page(g_st.active_page);
        mark_dirty();
    };

    card* keys = make_card(page, "SHORTCUTS");

    ui::box* kh = make_list_row(keys);
    add_caption(kh, "CHORD", ui::length::fixed(130));
    add_caption(kh, "PROGRAM", ui::length::flex());

    for (uint32_t i = 0; i < g_st.conf.shortcut_count; i++) {
        stlxconf_shortcut_t* sc = &g_st.conf.shortcuts[i];

        ui::box* row = make_list_row(keys);
        ui::text_input* chord = row->add<ui::text_input>();
        chord->s().main = ui::length::fixed(130);
        chord->set_text(sc->key);
        chord->on_change = [sc](const std::string& s) {
            put_str(sc->key, sizeof(sc->key), s);
            mark_dirty();
        };

        ui::text_input* path = row->add<ui::text_input>();
        path->s().main = ui::length::flex();
        path->set_text(sc->path);
        path->on_change = [sc](const std::string& s) {
            put_str(sc->path, sizeof(sc->path), s);
            mark_dirty();
        };
    }
}

/* Swaps the page subtree under the fixed chrome and re-lays it out */
static void build_page(uint32_t index) {
    g_st.active_page = index;
    for (uint32_t i = 0; i < PAGE_COUNT; i++) {
        g_st.nav[i]->set_selected(i == index);
    }
    g_st.page_title->set_text(PAGE_NAMES[index]);
    g_st.page_blurb->set_text(PAGE_BLURBS[index]);

    if (g_st.page) {
        g_st.page_slot->remove(g_st.page);
    }

    g_st.page = g_st.page_slot->add<ui::box>(ui::axis::column);
    g_st.page->s().main = ui::length::content();
    g_st.page->s().gap = 14;

    switch (index) {
    case 0: build_appearance(g_st.page); break;
    case 1: build_dock(g_st.page); break;
    case 2: build_input(g_st.page); break;
    default: build_startup(g_st.page); break;
    }

    g_st.page_slot->invalidate_layout();
}

/* Clamp the fields a broken save could take the desktop down with */
static void sanitize_conf() {
    stlxconf_t& c = g_st.conf;

    if (c.bar_font_size < 8) c.bar_font_size = 8;
    if (c.bar_font_size > 48) c.bar_font_size = 48;
    if (c.taskbar_height < 24) c.taskbar_height = 24;
    if (c.taskbar_height > 128) c.taskbar_height = 128;
    if (c.taskbar_icon_size < 16) c.taskbar_icon_size = 16;
    if (c.taskbar_icon_size > 96) c.taskbar_icon_size = 96;
    if (c.taskbar_spacing > 64) c.taskbar_spacing = 64;
    if (c.key_repeat_delay_ms != 0 && c.key_repeat_interval_ms == 0) {
        c.key_repeat_interval_ms = 40;
    }
}

/* Delivers the reload signal to the display manager's pidfile */
static void notify_dm() {
    FILE* f = fopen(DM_PID_PATH, "r");
    if (!f) {
        return;
    }

    int pid = 0;
    int got = fscanf(f, "%d", &pid);
    fclose(f);

    if (got == 1 && pid > 1) {
        kill(pid, SIGHUP);
    }
}

static void save_conf() {
    sanitize_conf();

    if (stlxconf_save(&g_st.conf, STLXCONF_PATH) != 0) {
        set_status("could not write the config file");
        return;
    }

    notify_dm();
    g_st.dirty = false;
    set_status("saved, desktop updated");
    build_page(g_st.active_page);
}

static void revert_conf() {
    stlxconf_load(&g_st.conf, STLXCONF_PATH);
    g_st.dirty = false;
    set_status("reverted to the file on disk");
    build_page(g_st.active_page);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    stlxconf_load(&g_st.conf, STLXCONF_PATH);

    ui::app app("settings");
    if (!app.ok()) {
        printf("stlxsettings: no display manager\r\n");
        return 1;
    }

    g_st.win = app.create_window(700, 500, "Settings",
                                 STLXWIN_WF_RESIZABLE);
    if (!g_st.win) {
        printf("stlxsettings: window creation failed\r\n");
        return 1;
    }
    stlxwin_window_set_min_size(g_st.win->window(), 600, 420);

    const ui::theme& t = ui::theme::active();
    auto root = std::make_unique<ui::box>(ui::axis::row);
    root->s().background = t.window_bg;

    /* The navigation rail */
    ui::box* rail = root->add<ui::box>(ui::axis::column);
    rail->s().main = ui::length::fixed(RAIL_W);
    rail->s().background = RAIL_BG;
    rail->s().padding = ui::edge_insets::all(12);
    rail->s().gap = 4;

    ui::label* brand = rail->add<ui::label>("Settings");
    brand->set_font_size(17);
    brand->s().main = ui::length::content();
    brand->s().padding = ui::edge_insets::xy(4, 6);

    for (uint32_t i = 0; i < PAGE_COUNT; i++) {
        nav_item* item = rail->add<nav_item>(PAGE_NAMES[i]);
        item->s().main = ui::length::fixed(NAV_H);
        item->on_select = [i]() { build_page(i); };
        g_st.nav[i] = item;
    }

    ui::box* rail_spacer = rail->add<ui::box>();
    rail_spacer->s().main = ui::length::flex();

    ui::label* version = rail->add<ui::label>("Stellux 3.0");
    version->set_font_size(SMALL_PX);
    version->set_color(t.text_dim);
    version->s().main = ui::length::content();
    version->s().padding = ui::edge_insets::xy(4, 4);

    /* The content column: header, the scrolling page, footer */
    ui::box* content = root->add<ui::box>(ui::axis::column);
    content->s().main = ui::length::flex();

    ui::box* header = content->add<ui::box>(ui::axis::column);
    header->s().main = ui::length::content();
    header->s().padding = { 18, 20, 10, 20 };
    header->s().gap = 4;

    g_st.page_title = header->add<ui::label>("");
    g_st.page_title->set_font_size(TITLE_PX);
    g_st.page_title->s().main = ui::length::content();

    g_st.page_blurb = header->add<ui::label>("");
    g_st.page_blurb->set_font_size(SMALL_PX);
    g_st.page_blurb->set_color(t.text_dim);
    g_st.page_blurb->s().main = ui::length::content();

    ui::scroll_view* scroll = content->add<ui::scroll_view>();
    scroll->s().main = ui::length::flex();

    g_st.page_slot = scroll->add<ui::box>(ui::axis::column);
    g_st.page_slot->s().main = ui::length::content();
    g_st.page_slot->s().padding = { 4, 20, 16, 20 };

    ui::box* footer = content->add<ui::box>(ui::axis::row);
    footer->s().main = ui::length::content();
    footer->s().padding = ui::edge_insets::xy(20, 12);
    footer->s().gap = 8;
    footer->s().align_items = ui::align::center;

    g_st.status = footer->add<ui::label>("");
    g_st.status->set_font_size(SMALL_PX);
    g_st.status->set_color(t.text_dim);
    g_st.status->s().main = ui::length::content();

    ui::box* footer_spacer = footer->add<ui::box>();
    footer_spacer->s().main = ui::length::flex();

    ui::button* revert = footer->add<ui::button>("Revert");
    revert->s().main = ui::length::content();
    revert->on_click = revert_conf;

    ui::button* save = footer->add<ui::button>("Save");
    save->s().main = ui::length::content();
    save->set_accent(true);
    save->on_click = save_conf;

    g_st.win->set_root(std::move(root));
    build_page(0);

    g_st.win->on_close = [&app]() {
        app.quit(0);
        return true;
    };

    return app.run();
}
