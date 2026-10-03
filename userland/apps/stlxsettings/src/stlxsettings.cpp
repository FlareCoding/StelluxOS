/* stlxsettings: the desktop configuration editor. A navigation rail
 * selects a page of grouped cards, edits land in the config struct
 * as they happen, and saving writes the file back and signals the
 * display manager to re-read it live.
 */
#include <stlxconf/conf.h>

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

constexpr const char* DM_PID_PATH = "/tmp/stlxdm.pid";

/* App palette on top of the theme: a darker rail and raised cards */
static const QColor RAIL_BG(0x181825);
static const QColor RAIL_HOVER(0x232334);
static const QColor CARD_BG(0x252536);

constexpr int WINDOW_W = 700;
constexpr int WINDOW_H = 500;
constexpr int WINDOW_MIN_W = 600;
constexpr int WINDOW_MIN_H = 420;
constexpr int RAIL_W = 172;
constexpr int NAV_H = 32;
constexpr int NAV_RADIUS = 6;
constexpr int NAV_TEXT_X = 14;
constexpr int CARD_RADIUS = 10;
constexpr int CARD_PADDING = 16;
constexpr int ROW_LABEL_W = 150;
constexpr int SWATCH_PX = 28;
constexpr int MENU_BUTTON_W = 36;
constexpr int BRAND_PX = 14;
constexpr int TITLE_PX = 16;
constexpr int CARD_TITLE_PX = 10;
constexpr int SMALL_PX = 10;

constexpr uint32_t REPEAT_DELAY_MS = 400;
constexpr uint32_t REPEAT_INTERVAL_MS = 40;

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
class nav_item : public QWidget {
public:
    explicit nav_item(const QString& text) : m_text(text) {
        setFixedHeight(NAV_H);
        setAttribute(Qt::WA_Hover);
    }

    std::function<void()> on_select;

    void set_selected(bool selected) {
        if (m_selected == selected) {
            return;
        }

        m_selected = selected;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);

        if (m_selected) {
            p.setBrush(palette().color(QPalette::Button));
            p.drawRoundedRect(rect(), NAV_RADIUS, NAV_RADIUS);
            p.setBrush(palette().color(QPalette::Highlight));
            p.drawRoundedRect(QRect(0, 8, 3, height() - 16), 2, 2);
        } else if (underMouse()) {
            p.setBrush(RAIL_HOVER);
            p.drawRoundedRect(rect(), NAV_RADIUS, NAV_RADIUS);
        }

        p.setPen(palette().color(m_selected ? QPalette::WindowText : QPalette::PlaceholderText));
        p.drawText(rect().adjusted(NAV_TEXT_X, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m_text);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (rect().contains(event->position().toPoint()) && on_select) {
            on_select();
        }
    }

private:
    QString m_text;
    bool m_selected = false;
};

/* A raised rounded panel grouping related settings under a caption */
class card : public QWidget {
public:
    card() {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
        layout->setSpacing(10);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(CARD_BG);
        p.drawRoundedRect(rect(), CARD_RADIUS, CARD_RADIUS);
    }
};

/* A live preview of a color field's value */
class swatch : public QWidget {
public:
    explicit swatch(const uint32_t* value) : m_value(value) {
        setFixedSize(SWATCH_PX, SWATCH_PX);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Midlight));
        p.drawRoundedRect(rect(), 6, 6);
        p.setBrush(QColor(*m_value & 0xFFFFFFu));
        p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6);
    }

private:
    const uint32_t* m_value;
};

/* Everything the pages edit, shared by the builders and the footer */
struct app_state {
    stlxconf_t conf;
    QWidget* page_slot = nullptr;
    QWidget* page = nullptr;
    QLabel* page_title = nullptr;
    QLabel* page_blurb = nullptr;
    QLabel* status = nullptr;
    nav_item* nav[PAGE_COUNT] = {};
    uint32_t active_page = 0;
    bool dirty = false;
};

} // namespace

static app_state g_st;

static void build_page(uint32_t index);

static QFont font_px(int pixels) {
    QFont font = QApplication::font();
    font.setPixelSize(pixels);
    return font;
}

static void set_dim(QWidget* widget) {
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    widget->setPalette(palette);
}

static void set_status(const char* text) {
    if (g_st.status) {
        g_st.status->setText(QString::fromUtf8(text));
    }
}

static void mark_dirty() {
    if (!g_st.dirty) {
        g_st.dirty = true;
        set_status("unsaved changes");
    }
}

/* Pages rebuild once the event that asked for it is done, since its widget goes away */
static void rebuild_page_later() {
    QTimer::singleShot(0, [] { build_page(g_st.active_page); });
}

static QString format_hex(uint32_t value) {
    return QString::asprintf("0x%08X", value);
}

/* Bounded copy for the conf's fixed char arrays */
static void put_str(char* dst, size_t cap, const QString& src) {
    QByteArray utf8 = src.toUtf8();
    size_t len = static_cast<size_t>(utf8.size()) < cap - 1 ? static_cast<size_t>(utf8.size()) : cap - 1;
    memcpy(dst, utf8.constData(), len);
    dst[len] = '\0';
}

/* Drop-in entries follow the user's own in every list, so edits, moves and
 * additions stay within the first `owned` rows */
template <typename item_t>
static uint32_t owned_count(const item_t* items, uint32_t count) {
    uint32_t owned = 0;
    while (owned < count && !items[owned].from_drop_in) {
        owned++;
    }

    return owned;
}

/* Opens a slot at `at` by shifting the entries after it up by one */
template <typename item_t>
static item_t* insert_slot(item_t* items, uint32_t count, uint32_t at) {
    for (uint32_t i = count; i > at; i--) {
        items[i] = items[i - 1];
    }

    memset(&items[at], 0, sizeof(items[at]));
    return &items[at];
}

static uint32_t parse_u32(const QString& s, int base) {
    return static_cast<uint32_t>(strtoul(s.toUtf8().constData(), nullptr, base));
}

static QVBoxLayout* layout_of(QWidget* widget) {
    return static_cast<QVBoxLayout*>(widget->layout());
}

static QLabel* make_caption(const QString& text) {
    auto* caption = new QLabel(text);
    caption->setFont(font_px(CARD_TITLE_PX));
    caption->setContentsMargins(4, 0, 0, 0);
    set_dim(caption);
    return caption;
}

static card* make_card(QWidget* page, const char* title) {
    auto* c = new card;
    layout_of(c)->addWidget(make_caption(QString::fromUtf8(title)));
    layout_of(page)->addWidget(c);
    return c;
}

/* A labeled row inside a card: fixed label column, controls after it */
static QHBoxLayout* make_row(card* parent, const char* label) {
    auto* row = new QHBoxLayout;
    row->setSpacing(12);

    auto* name = new QLabel(QString::fromUtf8(label));
    name->setFixedWidth(ROW_LABEL_W);
    name->setContentsMargins(4, 0, 0, 0);
    row->addWidget(name);

    layout_of(parent)->addLayout(row);
    return row;
}

/* A bare row for list entries, no label column */
static QHBoxLayout* make_list_row(card* parent) {
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    layout_of(parent)->addLayout(row);
    return row;
}

/* Paths and commands read from the left, so a field opens on the start of its text */
static QLineEdit* make_field(const QString& text) {
    auto* field = new QLineEdit(text);
    field->setCursorPosition(0);
    return field;
}

/* A text field of fixed width, or one sharing the row's spare width when `width` is 0 */
static QLineEdit* add_field(QHBoxLayout* row, const char* text, int width) {
    QLineEdit* field = make_field(QString::fromUtf8(text));
    if (width > 0) {
        field->setFixedWidth(width);
        row->addWidget(field);
    } else {
        row->addWidget(field, 1);
    }

    return field;
}

/* One column of a row the user cannot edit */
static void add_value(QHBoxLayout* row, const char* text, int width) {
    auto* value = new QLabel(QString::fromUtf8(text));
    value->setContentsMargins(4, 0, 0, 0);
    if (width > 0) {
        value->setFixedWidth(width);
        row->addWidget(value);
    } else {
        row->addWidget(value, 1);
    }
}

/* One dim column caption inside a list header row */
static void add_caption(QHBoxLayout* header, const char* text, int width) {
    QLabel* caption = make_caption(QString::fromUtf8(text));
    if (width > 0) {
        caption->setFixedWidth(width);
        header->addWidget(caption);
    } else {
        header->addWidget(caption, 1);
    }
}

/* Takes the menu slot of a row that a drop-in file owns */
static void mark_drop_in_row(QHBoxLayout* row) {
    QLabel* caption = make_caption(QStringLiteral("drop-in"));
    caption->setFixedWidth(MENU_BUTTON_W + 20);
    row->addWidget(caption);
}

/* A field writing through to a char array */
static QLineEdit* add_text_field(QHBoxLayout* row, char* dst, size_t cap, int width) {
    QLineEdit* field = add_field(row, dst, width);
    QObject::connect(field, &QLineEdit::textEdited, [dst, cap](const QString& s) {
        put_str(dst, cap, s);
        mark_dirty();
    });

    return field;
}

/* A color field: hex input plus a live swatch previewing the value */
static void make_color_row(card* parent, const char* label, uint32_t* value) {
    QHBoxLayout* row = make_row(parent, label);

    QLineEdit* field = make_field(format_hex(*value));
    field->setFixedWidth(130);
    row->addWidget(field);

    auto* preview = new swatch(value);
    row->addWidget(preview);
    row->addStretch(1);

    QObject::connect(field, &QLineEdit::textEdited, [value, preview](const QString& s) {
        *value = parse_u32(s, 16);
        preview->update();
        mark_dirty();
    });
}

/* A short numeric field writing through to a config integer */
static void make_number_row(card* parent, const char* label, uint32_t* value) {
    QHBoxLayout* row = make_row(parent, label);

    QLineEdit* field = make_field(QString::number(*value));
    field->setFixedWidth(80);
    row->addWidget(field);
    row->addStretch(1);

    QObject::connect(field, &QLineEdit::textEdited, [value](const QString& s) {
        *value = parse_u32(s, 10);
        mark_dirty();
    });
}

/* A full width path or text field writing through to a char array */
static void make_text_row(card* parent, const char* label, char* dst, size_t cap) {
    add_text_field(make_row(parent, label), dst, cap, 0);
}

/* A "..." button opening the row actions for one list entry. The
 * mutation runs once the menu returns, then the page rebuilds. */
template <typename move_fn, typename remove_fn>
static void make_row_menu(QHBoxLayout* row, uint32_t index, uint32_t count, move_fn mover,
                          remove_fn remover) {
    auto* more = new QToolButton;
    more->setText(QStringLiteral("..."));
    more->setFixedWidth(MENU_BUTTON_W);
    row->addWidget(more);

    QObject::connect(more, &QToolButton::clicked, [more, index, count, mover, remover]() {
        QMenu menu(more);
        QAction* up = menu.addAction(QStringLiteral("Move up"));
        up->setEnabled(index > 0);
        QAction* down = menu.addAction(QStringLiteral("Move down"));
        down->setEnabled(index + 1 < count);
        menu.addSeparator();
        QAction* remove = menu.addAction(QStringLiteral("Remove"));

        QAction* chosen = menu.exec(more->mapToGlobal(QPoint(0, more->height())));
        if (!chosen) {
            return;
        }

        if (chosen == up) {
            mover(index, index - 1);
        } else if (chosen == down) {
            mover(index, index + 1);
        } else if (chosen == remove) {
            remover(index);
        }

        mark_dirty();
        rebuild_page_later();
    });
}

static void add_list_button(card* parent, const char* text, std::function<void()> on_click) {
    auto* button = new QPushButton(QString::fromUtf8(text));
    QObject::connect(button, &QPushButton::clicked, std::move(on_click));

    auto* row = new QHBoxLayout;
    row->addWidget(button);
    row->addStretch(1);
    layout_of(parent)->addLayout(row);
}

static void build_appearance(QWidget* page) {
    card* wall = make_card(page, "WALLPAPER");
    make_text_row(wall, "Image path", g_st.conf.wallpaper, sizeof(g_st.conf.wallpaper));
    make_color_row(wall, "Fallback color", &g_st.conf.bg_color);

    card* colors = make_card(page, "BAR COLORS");
    make_color_row(colors, "Bar", &g_st.conf.bar_color);
    make_color_row(colors, "Accent", &g_st.conf.accent_color);
    make_color_row(colors, "Text", &g_st.conf.text_color);
    make_number_row(colors, "Bar font size", &g_st.conf.bar_font_size);
}

static void build_dock(QWidget* page) {
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

    QHBoxLayout* header = make_list_row(pins);
    add_caption(header, "LABEL", 110);
    add_caption(header, "PROGRAM", 0);
    add_caption(header, "ARGUMENTS", 130);
    add_caption(header, "ICON", 0);
    header->addSpacing(MENU_BUTTON_W + 20);

    for (uint32_t i = 0; i < g_st.conf.pin_count; i++) {
        stlxconf_pin_t* pin = &g_st.conf.pins[i];

        QHBoxLayout* row = make_list_row(pins);
        if (pin->from_drop_in) {
            add_value(row, pin->label, 110);
            add_value(row, pin->path, 0);
            add_value(row, pin->args, 130);
            add_value(row, pin->icon_path, 0);
            mark_drop_in_row(row);
            continue;
        }

        add_text_field(row, pin->label, sizeof(pin->label), 110);
        add_text_field(row, pin->path, sizeof(pin->path), 0);
        add_text_field(row, pin->args, sizeof(pin->args), 130);
        add_text_field(row, pin->icon_path, sizeof(pin->icon_path), 0);
        make_row_menu(row, i, owned_count(g_st.conf.pins, g_st.conf.pin_count), move_pin, remove_pin);
    }

    add_list_button(pins, "Add launcher", [] {
        if (g_st.conf.pin_count >= STLXCONF_MAX_TASKBAR) {
            set_status("launcher table is full");
            return;
        }

        uint32_t owned = owned_count(g_st.conf.pins, g_st.conf.pin_count);
        stlxconf_pin_t* pin = insert_slot(g_st.conf.pins, g_st.conf.pin_count, owned);
        snprintf(pin->name, sizeof(pin->name), "pin%u", owned);
        put_str(pin->label, sizeof(pin->label), QStringLiteral("New app"));
        g_st.conf.pin_count++;

        mark_dirty();
        rebuild_page_later();
    });
}

static void build_input(QWidget* page) {
    card* repeat = make_card(page, "KEYBOARD REPEAT");

    auto* enabled = new QCheckBox(QStringLiteral("Repeat held keys"));
    enabled->setChecked(g_st.conf.key_repeat_delay_ms != 0);
    layout_of(repeat)->addWidget(enabled);
    QObject::connect(enabled, &QCheckBox::toggled, [](bool on) {
        g_st.conf.key_repeat_delay_ms = on ? REPEAT_DELAY_MS : 0;
        mark_dirty();
        rebuild_page_later();
    });

    if (g_st.conf.key_repeat_delay_ms != 0) {
        make_number_row(repeat, "Delay (ms)", &g_st.conf.key_repeat_delay_ms);
        make_number_row(repeat, "Interval (ms)", &g_st.conf.key_repeat_interval_ms);
    }
}

static void build_startup(QWidget* page) {
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

    QHBoxLayout* header = make_list_row(autos);
    add_caption(header, "PROGRAM", 0);
    add_caption(header, "ARGUMENTS", 130);
    header->addSpacing(MENU_BUTTON_W + 20);

    for (uint32_t i = 0; i < g_st.conf.autostart_count; i++) {
        stlxconf_autostart_t* as = &g_st.conf.autostart[i];

        QHBoxLayout* row = make_list_row(autos);
        if (as->from_drop_in) {
            add_value(row, as->path, 0);
            add_value(row, as->args, 130);
            mark_drop_in_row(row);
            continue;
        }

        add_text_field(row, as->path, sizeof(as->path), 0);
        add_text_field(row, as->args, sizeof(as->args), 130);
        make_row_menu(row, i, owned_count(g_st.conf.autostart, g_st.conf.autostart_count), move_auto,
                      remove_auto);
    }

    add_list_button(autos, "Add program", [] {
        if (g_st.conf.autostart_count >= STLXCONF_MAX_AUTOSTART) {
            set_status("autostart table is full");
            return;
        }

        uint32_t owned = owned_count(g_st.conf.autostart, g_st.conf.autostart_count);
        stlxconf_autostart_t* as = insert_slot(g_st.conf.autostart, g_st.conf.autostart_count, owned);
        snprintf(as->name, sizeof(as->name), "entry%u", owned);
        g_st.conf.autostart_count++;

        mark_dirty();
        rebuild_page_later();
    });

    card* keys = make_card(page, "SHORTCUTS");

    QHBoxLayout* kh = make_list_row(keys);
    add_caption(kh, "CHORD", 130);
    add_caption(kh, "PROGRAM", 0);

    for (uint32_t i = 0; i < g_st.conf.shortcut_count; i++) {
        stlxconf_shortcut_t* sc = &g_st.conf.shortcuts[i];

        QHBoxLayout* row = make_list_row(keys);
        if (sc->from_drop_in) {
            add_value(row, sc->key, 130);
            add_value(row, sc->path, 0);
            mark_drop_in_row(row);
            continue;
        }

        add_text_field(row, sc->key, sizeof(sc->key), 130);
        add_text_field(row, sc->path, sizeof(sc->path), 0);
    }
}

/* Swaps the page under the fixed chrome */
static void build_page(uint32_t index) {
    g_st.active_page = index;
    for (uint32_t i = 0; i < PAGE_COUNT; i++) {
        g_st.nav[i]->set_selected(i == index);
    }

    g_st.page_title->setText(QString::fromUtf8(PAGE_NAMES[index]));
    g_st.page_blurb->setText(QString::fromUtf8(PAGE_BLURBS[index]));

    delete g_st.page;
    g_st.page = new QWidget;
    auto* page_layout = new QVBoxLayout(g_st.page);
    page_layout->setContentsMargins(0, 0, 0, 0);
    page_layout->setSpacing(14);

    switch (index) {
    case 0: build_appearance(g_st.page); break;
    case 1: build_dock(g_st.page); break;
    case 2: build_input(g_st.page); break;
    default: build_startup(g_st.page); break;
    }

    page_layout->addStretch(1);
    layout_of(g_st.page_slot)->addWidget(g_st.page);
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
        c.key_repeat_interval_ms = REPEAT_INTERVAL_MS;
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

static void load_conf() {
    stlxconf_load(&g_st.conf, STLXCONF_PATH);
    stlxconf_load_drop_ins(&g_st.conf, STLXCONF_DROP_IN_DIR);
}

static void save_conf() {
    sanitize_conf();

    if (stlxconf_save(&g_st.conf, STLXCONF_PATH) != 0) {
        set_status("could not write the config file");
        return;
    }

    notify_dm();

    /* Reloading shows the entries in the order the desktop loaded them, drop-ins last */
    load_conf();
    g_st.dirty = false;
    set_status("saved, desktop updated");
    build_page(g_st.active_page);
}

static void revert_conf() {
    load_conf();
    g_st.dirty = false;
    set_status("reverted to the file on disk");
    build_page(g_st.active_page);
}

static QWidget* make_rail() {
    auto* rail = new QWidget;
    rail->setFixedWidth(RAIL_W);
    rail->setAutoFillBackground(true);
    QPalette palette = rail->palette();
    palette.setColor(QPalette::Window, RAIL_BG);
    rail->setPalette(palette);

    auto* layout = new QVBoxLayout(rail);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(4);

    auto* brand = new QLabel(QStringLiteral("Settings"));
    brand->setFont(font_px(BRAND_PX));
    brand->setContentsMargins(4, 6, 4, 6);
    layout->addWidget(brand);

    for (uint32_t i = 0; i < PAGE_COUNT; i++) {
        auto* item = new nav_item(QString::fromUtf8(PAGE_NAMES[i]));
        item->on_select = [i]() { build_page(i); };
        g_st.nav[i] = item;
        layout->addWidget(item);
    }

    layout->addStretch(1);

    auto* version = new QLabel(QStringLiteral("Stellux 3.0"));
    version->setFont(font_px(SMALL_PX));
    version->setContentsMargins(4, 4, 4, 4);
    set_dim(version);
    layout->addWidget(version);

    return rail;
}

static QWidget* make_header() {
    auto* header = new QWidget;
    auto* layout = new QVBoxLayout(header);
    layout->setContentsMargins(20, 18, 20, 10);
    layout->setSpacing(4);

    g_st.page_title = new QLabel;
    g_st.page_title->setFont(font_px(TITLE_PX));
    layout->addWidget(g_st.page_title);

    g_st.page_blurb = new QLabel;
    g_st.page_blurb->setFont(font_px(SMALL_PX));
    set_dim(g_st.page_blurb);
    layout->addWidget(g_st.page_blurb);

    return header;
}

static QWidget* make_footer() {
    auto* footer = new QWidget;
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(20, 12, 20, 12);
    layout->setSpacing(8);

    g_st.status = new QLabel;
    g_st.status->setFont(font_px(SMALL_PX));
    set_dim(g_st.status);
    layout->addWidget(g_st.status);
    layout->addStretch(1);

    auto* revert = new QPushButton(QStringLiteral("Revert"));
    QObject::connect(revert, &QPushButton::clicked, revert_conf);
    layout->addWidget(revert);

    /* The accent marks the action that writes the file */
    auto* save = new QPushButton(QStringLiteral("Save"));
    QPalette palette = save->palette();
    palette.setColor(QPalette::Button, palette.color(QPalette::Highlight));
    palette.setColor(QPalette::ButtonText, palette.color(QPalette::HighlightedText));
    save->setPalette(palette);
    QObject::connect(save, &QPushButton::clicked, save_conf);
    layout->addWidget(save);

    return footer;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    load_conf();

    QApplication app(argc, argv);

    QWidget window;
    window.setWindowTitle(QStringLiteral("Settings"));
    window.resize(WINDOW_W, WINDOW_H);
    window.setMinimumSize(WINDOW_MIN_W, WINDOW_MIN_H);

    auto* root = new QHBoxLayout(&window);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(make_rail());

    /* The content column: header, the scrolling page, footer */
    auto* content = new QVBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    content->addWidget(make_header());

    g_st.page_slot = new QWidget;
    auto* slot_layout = new QVBoxLayout(g_st.page_slot);
    slot_layout->setContentsMargins(20, 4, 20, 16);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(g_st.page_slot);
    content->addWidget(scroll, 1);
    content->addWidget(make_footer());
    root->addLayout(content, 1);

    build_page(0);
    window.show();

    return app.exec();
}
