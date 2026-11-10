/* stlxsettings: the desktop configuration editor. Setting rows show the config key each writes. Save writes the
 * file and signals the display manager to re-read it, while the Displays page's Apply sends its own requests. */
#include "kit.hpp"
#include "settings.hpp"

#include <stlxconf/conf.h>
#include <stlxtheme/theme.h>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <sys/utsname.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>

constexpr const char* DM_PID_PATH = "/tmp/stlxdm.pid";
constexpr const char* LAYOUTS_PATH = "/etc/stlxdm/layouts.conf";

/* The window is 960 by 680 with the title bar the display manager draws, not counting the 1 px border it adds */
constexpr int WINDOW_W = 960;
constexpr int WINDOW_OUTER_H = 680;
constexpr int WINDOW_H = WINDOW_OUTER_H - STLXTHEME_TITLE_BAR_HEIGHT;
constexpr int WINDOW_MIN_W = 800;
constexpr int WINDOW_MIN_H = 480;

constexpr int SIDEBAR_W = 220;
constexpr int SIDEBAR_PADDING_X = 8;
constexpr int GROUP_PADDING_TOP = 14;
constexpr int GROUP_HEAD_PADDING_X = 10;
constexpr int GROUP_HEAD_PADDING_BOTTOM = 6;
constexpr int NAV_ROW_GAP = 2;
constexpr int NAV_ROW_PADDING_X = 10;
constexpr int NAV_ROW_GLYPH_GAP = 10;
constexpr int MARKER_TOP = (STLXTHEME_CONTROL_HEIGHT - STLXTHEME_MARKER_HEIGHT) / 2;
constexpr int MARKER_RADIUS = 1;
constexpr int SIDEBAR_FOOT_H = 48;
constexpr int SIDEBAR_FOOT_PADDING_X = 18;
constexpr int SIDEBAR_FOOT_GAP = 8;
constexpr int LOGO_SIZE = 12;

constexpr int TITLE_LINE_HEIGHT = 36;
constexpr qreal TITLE_TRACKING_EM = -0.02;
constexpr int FOOTER_GAP = 8;

constexpr int LIST_ROW_H = 44;
constexpr int LIST_ROW_PADDING_X = 12;
constexpr int EDITOR_FIELD_W = 360;

stlxconf_t g_conf;

enum page_index : uint32_t { APPEARANCE_PAGE, DOCK_PAGE, DISPLAY_PAGE, INPUT_PAGE, STARTUP_PAGE };

constexpr uint32_t PAGE_COUNT = STARTUP_PAGE + 1;

static const char* const PAGE_NAMES[] = {
    "Appearance", "Dock", "Displays", "Input", "Startup"
};
static_assert(std::size(PAGE_NAMES) == PAGE_COUNT);

/* The names `--page` takes */
static const char* const PAGE_SLUGS[] = {
    "appearance", "dock", "displays", "input", "startup"
};
static_assert(std::size(PAGE_SLUGS) == PAGE_COUNT);

static const char* const PAGE_BLURBS[] = {
    "Wallpaper and glass.",
    "The apps pinned to the dock.",
    "Where each output sits, which one is main, and each one's mode.",
    "How held keys repeat.",
    "Programs that start with the desktop, and the shortcuts that launch programs.",
};
static_assert(std::size(PAGE_BLURBS) == PAGE_COUNT);

/* The file that stores each page's settings, for the Displays page the layout file that stlxdm writes */
static const char* const PAGE_FILES[] = {
    STLXCONF_PATH, STLXCONF_PATH, LAYOUTS_PATH, STLXCONF_PATH, STLXCONF_PATH
};
static_assert(std::size(PAGE_FILES) == PAGE_COUNT);

static const glyph PAGE_GLYPHS[] = {
    glyph::WALLPAPER, glyph::DOCK, glyph::DISPLAY, glyph::KEYBOARD, glyph::POWER
};
static_assert(std::size(PAGE_GLYPHS) == PAGE_COUNT);

/* A group runs from its first page to the next group's first, so its pages must be adjacent in `page_index` */
static const char* const GROUP_TITLES[] = { "Personalize", "Hardware", "System" };
static const uint32_t GROUP_FIRST_PAGES[] = { APPEARANCE_PAGE, DISPLAY_PAGE, STARTUP_PAGE };
static_assert(std::size(GROUP_FIRST_PAGES) == std::size(GROUP_TITLES));

namespace {

class nav_row : public QWidget {
public:
    nav_row(glyph icon, const QString& name) : m_icon(icon), m_name(name) {
        setFixedHeight(STLXTHEME_CONTROL_HEIGHT);
        setAttribute(Qt::WA_Hover);
    }

    std::function<void()> on_select;

    void set_current(bool is_current) {
        m_is_current = is_current;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        bool is_lit = m_is_current || underMouse();

        if (is_lit) {
            painter.setBrush(theme_color(m_is_current ? STLXTHEME_SURFACE_SELECTED : STLXTHEME_SURFACE_HOVER));
            painter.drawRoundedRect(QRectF(rect()), STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
        }

        /* Starts left of the row's edge, so only the marker's right corners show rounded */
        if (m_is_current) {
            painter.setBrush(theme_color(STLXTHEME_ACCENT));
            painter.drawRoundedRect(
                QRectF(-MARKER_RADIUS, MARKER_TOP, STLXTHEME_MARKER_WIDTH + MARKER_RADIUS, STLXTHEME_MARKER_HEIGHT),
                MARKER_RADIUS,
                MARKER_RADIUS
            );
        }

        QRectF glyph_box(
            NAV_ROW_PADDING_X,
            (height() - STLXTHEME_GLYPH_SIZE) / 2.0,
            STLXTHEME_GLYPH_SIZE,
            STLXTHEME_GLYPH_SIZE
        );
        paint_glyph(painter, m_icon, glyph_box, theme_color(is_lit ? STLXTHEME_TEXT : STLXTHEME_TEXT_DIM));

        int text_left = NAV_ROW_PADDING_X + STLXTHEME_GLYPH_SIZE + NAV_ROW_GLYPH_GAP;
        painter.setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::Medium));
        painter.setPen(theme_color(is_lit ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY));
        painter.drawText(rect().adjusted(text_left, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m_name);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (rect().contains(event->position().toPoint()) && on_select) {
            on_select();
        }
    }

private:
    glyph m_icon;
    QString m_name;
    bool m_is_current = false;
};

class sidebar : public QWidget {
public:
    sidebar(const QString& release, const QString& machine) : m_release(release), m_machine(machine) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), theme_color(STLXTHEME_SHADE));
        painter.fillRect(QRect(width() - 1, 0, 1, height()), theme_color(STLXTHEME_LINE_SUBTLE));

        int foot_top = height() - SIDEBAR_FOOT_H;
        QRectF logo_box(SIDEBAR_FOOT_PADDING_X, foot_top + (SIDEBAR_FOOT_H - LOGO_SIZE) / 2.0, LOGO_SIZE, LOGO_SIZE);
        paint_glyph(painter, glyph::LOGO, logo_box, theme_color(STLXTHEME_TEXT_DIM));

        QFont release_font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::Medium);
        QFontMetricsF release_metrics(release_font);
        qreal baseline = foot_top + (SIDEBAR_FOOT_H - release_metrics.height()) / 2 + release_metrics.ascent();
        qreal release_left = logo_box.right() + SIDEBAR_FOOT_GAP;

        painter.setFont(release_font);
        painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
        painter.drawText(QPointF(release_left, baseline), m_release);

        qreal machine_left = release_left + release_metrics.horizontalAdvance(m_release) + SIDEBAR_FOOT_GAP;
        painter.setFont(mono_font(STLXTHEME_CAPTION_SIZE));
        painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
        painter.drawText(QPointF(machine_left, baseline), m_machine);
    }

private:
    QString m_release;
    QString m_machine;
};

class page_head : public QWidget {
public:
    page_head() {
        setFixedHeight(PANE_MARGIN_TOP + TITLE_LINE_HEIGHT + BODY_LINE_HEIGHT);
    }

    void set_page(const QString& title, const QString& blurb, const QString& file) {
        m_title = title;
        m_blurb = blurb;
        m_file = file;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QFont title_font = ui_font(STLXTHEME_PAGE_TITLE_SIZE, QFont::DemiBold);
        title_font.setLetterSpacing(QFont::AbsoluteSpacing, TITLE_TRACKING_EM * STLXTHEME_PAGE_TITLE_SIZE);
        QFontMetricsF title_metrics(title_font);
        qreal baseline = PANE_MARGIN_TOP + (TITLE_LINE_HEIGHT - title_metrics.height()) / 2 + title_metrics.ascent();

        painter.setFont(title_font);
        painter.setPen(theme_color(STLXTHEME_TEXT));
        painter.drawText(QPointF(PANE_MARGIN_X, baseline), m_title);

        QRect blurb_line(
            PANE_MARGIN_X,
            PANE_MARGIN_TOP + TITLE_LINE_HEIGHT,
            width() - 2 * PANE_MARGIN_X,
            BODY_LINE_HEIGHT
        );
        painter.setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::Normal));
        painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
        painter.drawText(blurb_line, Qt::AlignVCenter | Qt::AlignLeft, m_blurb);

        QFont file_font = mono_font(STLXTHEME_CAPTION_SIZE);
        qreal file_width = QFontMetricsF(file_font).horizontalAdvance(m_file);
        painter.setFont(file_font);
        painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
        painter.drawText(QPointF(width() - PANE_MARGIN_X - file_width, baseline), m_file);
    }

private:
    QString m_title;
    QString m_blurb;
    QString m_file;
};

class footer_bar : public QWidget {
public:
    footer_bar() {
        setFixedHeight(STLXTHEME_FOOTER_HEIGHT);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(QRect(0, 0, width(), 1), theme_color(STLXTHEME_LINE_SUBTLE));
    }
};

/* The state of the window around the pages. `saved` holds the config as loaded, drop-ins included, so the footer
 * counts the changes Save would write. */
struct app_state {
    stlxconf_t saved;
    QScrollArea* scroll = nullptr;
    QWidget* page_slot = nullptr;
    QWidget* page = nullptr;
    page_head* head = nullptr;
    QLabel* status = nullptr;
    push_button* revert = nullptr;
    push_button* save = nullptr;
    nav_row* nav[PAGE_COUNT] = {};
    uint32_t active_page = 0;
    int chosen_scroll = 0; /* where the person last scrolled the page to */
};

} // namespace

static app_state g_st;

static void build_page(uint32_t index);

void set_status(const QString& text) {
    g_st.status->setText(text.left(1).toUpper() + text.mid(1));
}

static void note_change(bool is_changed, const QString& description, QStringList& changes) {
    if (is_changed) {
        changes.append(description);
    }
}

static bool same_entry(const stlxconf_pin_t& a, const stlxconf_pin_t& b) {
    return strcmp(a.label, b.label) == 0 && strcmp(a.path, b.path) == 0 && strcmp(a.args, b.args) == 0 &&
           strcmp(a.icon_path, b.icon_path) == 0;
}

static bool same_entry(const stlxconf_shortcut_t& a, const stlxconf_shortcut_t& b) {
    return strcmp(a.key, b.key) == 0 && strcmp(a.action, b.action) == 0 && strcmp(a.path, b.path) == 0 &&
           strcmp(a.args, b.args) == 0;
}

static bool same_entry(const stlxconf_autostart_t& a, const stlxconf_autostart_t& b) {
    return strcmp(a.path, b.path) == 0 && strcmp(a.args, b.args) == 0;
}

static QString entry_title(const stlxconf_pin_t& pin) {
    QString label = QString::fromUtf8(pin.label);
    return label.isEmpty() ? QStringLiteral("An app") : label;
}

static QString entry_title(const stlxconf_shortcut_t& shortcut) {
    QString key = QString::fromUtf8(shortcut.key);
    return key.isEmpty() ? QStringLiteral("A shortcut") : key;
}

static QString entry_title(const stlxconf_autostart_t& autostart) {
    return program_name(autostart.path, autostart.args);
}

/* Entries match by section name, which must be unique in a list, so a changed name reads as removed and added,
 * and a move counts once per list. Drop-in entries never differ, since the pages show them read only. */
template <typename item_t>
static void note_list_changes(
    const item_t* saved,
    uint32_t saved_count,
    const item_t* edited,
    uint32_t edited_count,
    const QString& list_name,
    QStringList& changes
) {
    int last_saved_position = -1;
    bool is_reordered = false;
    for (uint32_t i = 0; i < edited_count; i++) {
        int found = find_entry(saved, saved_count, edited[i].name);
        if (found < 0) {
            changes.append(entry_title(edited[i]) + QStringLiteral(" added"));
            continue;
        }

        note_change(!same_entry(saved[found], edited[i]), entry_title(edited[i]) + QStringLiteral(" changed"), changes);
        is_reordered = is_reordered || found < last_saved_position;
        last_saved_position = found;
    }

    for (uint32_t i = 0; i < saved_count; i++) {
        bool is_removed = find_entry(edited, edited_count, saved[i].name) < 0;
        note_change(is_removed, entry_title(saved[i]) + QStringLiteral(" removed"), changes);
    }

    note_change(is_reordered, list_name + QStringLiteral(" order changed"), changes);
}

bool is_key_repeat_on(const stlxconf_t& conf) {
    return conf.key_repeat_delay_ms != 0 && conf.key_repeat_interval_ms != 0;
}

/* What Save would write: one description for each changed setting, list entry and list order */
static QStringList pending_changes() {
    const stlxconf_t& saved = g_st.saved;
    const stlxconf_t& edited = g_conf;
    QStringList changes;

    note_change(strcmp(saved.wallpaper, edited.wallpaper) != 0, QStringLiteral("Wallpaper changed"), changes);
    note_change(saved.glass_level != edited.glass_level, QStringLiteral("Glass changed"), changes);
    note_change(saved.glass_tint != edited.glass_tint, QStringLiteral("Tint changed"), changes);

    bool was_repeating = is_key_repeat_on(saved);
    bool is_repeating = is_key_repeat_on(edited);
    bool is_delay_changed = was_repeating && is_repeating && saved.key_repeat_delay_ms != edited.key_repeat_delay_ms;
    bool is_interval_changed = saved.key_repeat_interval_ms != edited.key_repeat_interval_ms;

    note_change(was_repeating && !is_repeating, QStringLiteral("Key repeat turned off"), changes);
    note_change(!was_repeating && is_repeating, QStringLiteral("Key repeat turned on"), changes);
    note_change(is_delay_changed, QStringLiteral("Repeat delay changed"), changes);
    note_change(is_interval_changed, QStringLiteral("Repeat rate changed"), changes);

    note_list_changes(saved.pins, saved.pin_count, edited.pins, edited.pin_count, QStringLiteral("Dock"), changes);
    note_list_changes(
        saved.autostart,
        saved.autostart_count,
        edited.autostart,
        edited.autostart_count,
        QStringLiteral("Autostart"),
        changes
    );
    note_list_changes(
        saved.shortcuts,
        saved.shortcut_count,
        edited.shortcuts,
        edited.shortcut_count,
        QStringLiteral("Shortcut"),
        changes
    );

    return changes;
}

const stlxconf_t& saved_conf() {
    return g_st.saved;
}

void show_pending_changes() {
    QStringList changes = pending_changes();
    bool is_pending = !changes.isEmpty();
    g_st.revert->setEnabled(is_pending);
    g_st.save->setEnabled(is_pending);

    if (changes.size() == 1) {
        set_status(QStringLiteral("1 unsaved change: %1").arg(changes.first()));
    } else if (is_pending) {
        set_status(QStringLiteral("%1 unsaved changes").arg(changes.size()));
    } else {
        set_status(QStringLiteral("No unsaved changes"));
    }
}

void rebuild_page_later() {
    g_st.page->setEnabled(false);
    QTimer::singleShot(0, [] { build_page(g_st.active_page); });
}

void rebuild_displays_page_later() {
    if (g_st.active_page == DISPLAY_PAGE) {
        rebuild_page_later();
    }
}

void copy_string(char* destination, size_t capacity, const QString& text) {
    QByteArray utf8 = text.toUtf8();
    size_t length = qMin(static_cast<size_t>(utf8.size()), capacity - 1);
    memcpy(destination, utf8.constData(), length);
    destination[length] = '\0';
}

QWidget* make_field_row(
    const QString& label,
    const QString& key,
    char* destination,
    size_t capacity,
    std::function<void()> on_edit
) {
    auto* field = new QLineEdit(QString::fromUtf8(destination));
    field->setFont(mono_font(STLXTHEME_BODY_SIZE));
    field->setFixedWidth(EDITOR_FIELD_W);
    field->setCursorPosition(0);

    QObject::connect(field, &QLineEdit::textEdited, [destination, capacity, on_edit](const QString& text) {
        copy_string(destination, capacity, text);
        on_edit();
    });

    auto* row = new setting_row(label, key);
    row->add_control(field);

    return row;
}

void add_entry_menu(
    list_row* row,
    uint32_t index,
    uint32_t owned,
    std::function<void(uint32_t, uint32_t)> move,
    std::function<void(uint32_t)> remove
) {
    auto* more = new more_button;
    row->add_trailing(more);

    more->on_click = [more, index, owned, move, remove]() {
        QPointer<QMenu> menu = new QMenu(more);
        QAction* up = menu->addAction(QStringLiteral("Move up"));
        up->setEnabled(index > 0);
        QAction* down = menu->addAction(QStringLiteral("Move down"));
        down->setEnabled(index + 1 < owned);

        menu->addSeparator();
        QAction* removal = menu->addAction(QStringLiteral("Remove"));
        QAction* chosen = menu->exec(more->mapToGlobal(QPoint(0, more->height())));

        /* A page rebuilt while the menu runs its own event loop deletes the menu with its row */
        if (!menu) {
            return;
        }

        menu->deleteLater();

        if (chosen == up) {
            move(index, index - 1);
        } else if (chosen == down) {
            move(index, index + 1);
        } else if (chosen == removal) {
            remove(index);
        }

        if (chosen) {
            show_pending_changes();
            rebuild_page_later();
        }
    };
}

void make_openable(list_row* row, int index, int* open_index) {
    bool is_open = index == *open_index;
    row->set_open(is_open);
    row->on_click = [index, open_index, is_open]() {
        *open_index = is_open ? -1 : index;
        rebuild_page_later();
    };
}

/* The drop-in file that holds `[section]`, as conf.d/<file>, or conf.d when no readable .conf file holds it */
static QString drop_in_file_of(const QString& section) {
    QString header = QStringLiteral("[%1]").arg(section);
    QDir directory(QString::fromLatin1(STLXCONF_DROP_IN_DIR));
    for (const QFileInfo& file : directory.entryInfoList({ QStringLiteral("*.conf") }, QDir::Files, QDir::Name)) {
        QFile drop_in(file.filePath());
        if (!drop_in.open(QIODevice::ReadOnly)) {
            continue;
        }

        while (!drop_in.atEnd()) {
            if (QString::fromUtf8(drop_in.readLine()).trimmed() == header) {
                return QStringLiteral("conf.d/%1").arg(file.fileName());
            }
        }
    }

    return QStringLiteral("conf.d");
}

void lock_drop_in_row(list_row* row, const char* prefix, const char* name) {
    QString section = QStringLiteral("%1:%2").arg(QString::fromLatin1(prefix), QString::fromUtf8(name));
    row->set_locked(true);
    row->add_trailing(new chip(drop_in_file_of(section)));
}

push_button* make_add_button(const QString& text, bool is_room_left, std::function<void()> add) {
    auto* button = new push_button(text);
    button->set_small(true);
    button->set_glyph(glyph::PLUS);
    button->setEnabled(is_room_left);
    button->on_click = std::move(add);
    return button;
}

QWidget* make_editor() {
    auto* editor = new QWidget;
    auto* layout = new QVBoxLayout(editor);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    return editor;
}

QPixmap launcher_icon(const stlxconf_pin_t& pin) {
    QPixmap icon = pin.icon_path[0] ? QPixmap(QString::fromUtf8(pin.icon_path)) : QPixmap();
    if (icon.isNull()) {
        icon = QPixmap(QString::fromLatin1(STLXCONF_DEFAULT_ICON));
    }

    return icon;
}

QVBoxLayout* layout_of(QWidget* widget) {
    return static_cast<QVBoxLayout*>(widget->layout());
}

card* add_section(QWidget* page, const QString& title, const QString& config_section) {
    auto* section = new card;
    layout_of(page)->addWidget(new section_head(title, config_section));
    layout_of(page)->addWidget(section);
    return section;
}

static QHBoxLayout* make_row(card* parent, const char* label, const char* key) {
    auto* row = new setting_row(QString::fromUtf8(label), QString::fromLatin1(key));
    parent->add_row(row);
    return row->controls();
}

QHBoxLayout* make_list_row(card* parent) {
    auto* row = new QWidget;
    row->setMinimumHeight(LIST_ROW_H);
    parent->add_row(row);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(LIST_ROW_PADDING_X, 0, LIST_ROW_PADDING_X, 0);
    layout->setSpacing(FOOTER_GAP);

    return layout;
}

push_button* add_list_button(card* parent, const char* text, std::function<void()> on_click) {
    auto* button = new push_button(QString::fromUtf8(text));
    button->set_small(true);
    button->on_click = std::move(on_click);

    QHBoxLayout* row = make_list_row(parent);
    row->addWidget(button);
    row->addStretch(1);

    return button;
}

void add_choice_row(card* parent, const char* label, const char* key, QComboBox* choice) {
    QHBoxLayout* row = make_row(parent, label, key);
    row->addWidget(choice);
    row->addStretch(1);
}

/* Swaps the page under the fixed chrome */
static void build_page(uint32_t index) {
    if (index != g_st.active_page) {
        g_st.chosen_scroll = 0;
        g_st.scroll->verticalScrollBar()->setValue(0);
    }

    g_st.active_page = index;
    for (uint32_t i = 0; i < PAGE_COUNT; i++) {
        g_st.nav[i]->set_current(i == index);
    }

    g_st.head->set_page(
        QString::fromUtf8(PAGE_NAMES[index]),
        QString::fromUtf8(PAGE_BLURBS[index]),
        QString::fromLatin1(PAGE_FILES[index])
    );

    delete g_st.page;
    g_st.page = new QWidget;
    auto* page_layout = new QVBoxLayout(g_st.page);
    page_layout->setContentsMargins(PANE_MARGIN_X, 0, PANE_MARGIN_X, PANE_MARGIN_BOTTOM);
    page_layout->setSpacing(0);

    switch (static_cast<page_index>(index)) {
    case APPEARANCE_PAGE: build_appearance_page(g_st.page); break;
    case DOCK_PAGE: build_dock_page(g_st.page); break;
    case DISPLAY_PAGE: build_displays_page(g_st.page); break;
    case INPUT_PAGE: build_input_page(g_st.page); break;
    case STARTUP_PAGE: build_startup_page(g_st.page); break;
    }

    page_layout->addStretch(1);
    layout_of(g_st.page_slot)->addWidget(g_st.page);
}

/* A rebuild can empty the scroll bar's range for a moment, which puts the
 * page at its top, so each range change sets the person's last scroll */
static void track_page_scroll(QScrollBar* page_scroll_bar) {
    QObject::connect(page_scroll_bar, &QAbstractSlider::actionTriggered, [page_scroll_bar] {
        g_st.chosen_scroll = page_scroll_bar->sliderPosition();
    });

    QObject::connect(page_scroll_bar, &QAbstractSlider::rangeChanged, [page_scroll_bar] {
        page_scroll_bar->setValue(g_st.chosen_scroll);
    });
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
    stlxconf_load(&g_conf, STLXCONF_PATH);
    stlxconf_load_drop_ins(&g_conf, STLXCONF_DROP_IN_DIR);
    g_st.saved = g_conf;
}

static void save_conf() {
    if (stlxconf_save(&g_conf, STLXCONF_PATH) != 0) {
        set_status(QStringLiteral("Could not write the config file"));
        return;
    }

    notify_dm();

    /* Reloading shows the entries in the order the desktop loaded them, drop-ins last */
    load_conf();
    build_page(g_st.active_page);
    show_pending_changes();
    set_status(QStringLiteral("Saved, the desktop updated"));
}

static void revert_conf() {
    load_conf();
    build_page(g_st.active_page);
    show_pending_changes();
    set_status(QStringLiteral("Reverted to the file on disk"));
}

/* The release before its build suffix, 3.0.1 of 3.0.1-x86_64-release */
static QString release_version(const utsname& system) {
    return QString::fromUtf8(system.release).section(QLatin1Char('-'), 0, 0);
}

static QWidget* make_group(size_t group) {
    auto* box = new QWidget;
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, GROUP_PADDING_TOP, 0, 0);
    layout->setSpacing(NAV_ROW_GAP);

    QLabel* head = make_text(QString::fromUtf8(GROUP_TITLES[group]).toUpper(), caps_font(), STLXTHEME_TEXT_DIM);
    head->setContentsMargins(GROUP_HEAD_PADDING_X, 0, GROUP_HEAD_PADDING_X, GROUP_HEAD_PADDING_BOTTOM);
    head->setFixedHeight(CAPTION_LINE_HEIGHT + GROUP_HEAD_PADDING_BOTTOM);
    layout->addWidget(head);

    bool is_last = group + 1 == std::size(GROUP_TITLES);
    uint32_t end = is_last ? PAGE_COUNT : GROUP_FIRST_PAGES[group + 1];
    for (uint32_t i = GROUP_FIRST_PAGES[group]; i < end; i++) {
        auto* row = new nav_row(PAGE_GLYPHS[i], QString::fromUtf8(PAGE_NAMES[i]));
        row->on_select = [i]() { build_page(i); };
        g_st.nav[i] = row;
        layout->addWidget(row);
    }

    return box;
}

static QWidget* make_sidebar() {
    utsname system = {};
    uname(&system);

    QString release = QStringLiteral("Stellux %1").arg(release_version(system));
    auto* sidebar_widget = new sidebar(release, QString::fromUtf8(system.machine));
    sidebar_widget->setFixedWidth(SIDEBAR_W);
    auto* layout = new QVBoxLayout(sidebar_widget);
    layout->setContentsMargins(SIDEBAR_PADDING_X, 0, SIDEBAR_PADDING_X, SIDEBAR_FOOT_H);
    layout->setSpacing(0);

    for (size_t group = 0; group < std::size(GROUP_TITLES); group++) {
        layout->addWidget(make_group(group));
    }

    layout->addStretch(1);

    return sidebar_widget;
}

static QWidget* make_footer() {
    auto* footer = new footer_bar;
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(PANE_MARGIN_X, 0, PANE_MARGIN_X, 0);
    layout->setSpacing(FOOTER_GAP);

    g_st.status = make_text(QString(), ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal), STLXTHEME_TEXT_DIM);
    layout->addWidget(g_st.status, 1);

    g_st.revert = new push_button(QStringLiteral("Revert"));
    g_st.revert->on_click = revert_conf;
    layout->addWidget(g_st.revert);

    g_st.save = new push_button(QStringLiteral("Save"));
    g_st.save->set_primary(true);
    g_st.save->on_click = save_conf;
    layout->addWidget(g_st.save);

    return footer;
}

/* The page `--page <name>` names, the first page without the option, or -1 for a missing name or one no page has */
static int requested_page(const QStringList& arguments) {
    int option = static_cast<int>(arguments.indexOf(QStringLiteral("--page")));
    if (option < 0) {
        return APPEARANCE_PAGE;
    }

    QString name = arguments.value(option + 1);
    for (uint32_t i = 0; i < PAGE_COUNT; i++) {
        if (name == QLatin1String(PAGE_SLUGS[i])) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

static void print_usage() {
    QStringList names;
    for (const char* slug : PAGE_SLUGS) {
        names.append(QLatin1String(slug));
    }

    fprintf(stderr, "usage: stlxsettings [--page %s]\n", names.join(QLatin1Char('|')).toUtf8().constData());
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    QApplication app(argc, argv);
    int first_page = requested_page(app.arguments());
    if (first_page < 0) {
        print_usage();
        return 1;
    }

    load_conf();

    QWidget window;
    window.setWindowTitle(QStringLiteral("Settings"));
    window.resize(WINDOW_W, WINDOW_H);
    window.setMinimumSize(WINDOW_MIN_W, WINDOW_MIN_H);

    auto* root = new QHBoxLayout(&window);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(make_sidebar());

    /* The content column: the page head, a trial's keep prompt, the scrolling page, the footer */
    auto* content = new QVBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    g_st.head = new page_head;
    content->addWidget(g_st.head);
    content->addWidget(make_keep_prompt());

    g_st.page_slot = new QWidget;
    auto* slot_layout = new QVBoxLayout(g_st.page_slot);
    slot_layout->setContentsMargins(0, 0, 0, 0);

    /* The scroll area fills its viewport and the widget it holds, which would hide the glass behind the cards */
    g_st.scroll = new QScrollArea;
    g_st.scroll->setWidgetResizable(true);
    g_st.scroll->setFrameShape(QFrame::NoFrame);
    g_st.scroll->setWidget(g_st.page_slot);
    g_st.scroll->viewport()->setAutoFillBackground(false);
    g_st.page_slot->setAutoFillBackground(false);

    track_page_scroll(g_st.scroll->verticalScrollBar());

    content->addWidget(g_st.scroll, 1);
    content->addWidget(make_footer());
    root->addLayout(content, 1);

    build_page(static_cast<uint32_t>(first_page));
    show_pending_changes();
    window.show();

    return app.exec();
}
