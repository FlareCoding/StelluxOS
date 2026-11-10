/* stlxsettings: the desktop configuration editor. A navigation rail
 * selects a page of grouped cards. Edits but the Display page's land
 * in the config struct, and saving writes the file and signals the
 * display manager to re-read it live. That page's Apply sends its own.
 */
#include <stlxconf/conf.h>
#include <stlxwin/stlxwin.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSocketNotifier>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <vector>

constexpr const char* DM_PID_PATH = "/tmp/stlxdm.pid";

/* App palette on top of the theme: the rail's hover and raised cards */
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
constexpr int ARRANGEMENT_H = 220;
constexpr int ARRANGEMENT_MARGIN = 16;
constexpr int OUTPUT_RADIUS = 6;
constexpr int MAIN_STRIP_PX = 4;
constexpr int MAIN_STRIP_INSET = 3;
constexpr int SNAP_PX = 24;

constexpr uint32_t REPEAT_DELAY_MS = 400;
constexpr uint32_t REPEAT_INTERVAL_MS = 40;

constexpr uint64_t MILLIHERTZ_PER_HERTZ = 1000;
constexpr int COUNTDOWN_TICK_MS = 1000;

/* The display manager sets a trial's mode back this long after lighting it, unless it is kept */
constexpr uint32_t TRIAL_SECONDS = 15;

enum page_index : uint32_t { APPEARANCE_PAGE, DOCK_PAGE, DISPLAY_PAGE, INPUT_PAGE, STARTUP_PAGE };

constexpr uint32_t PAGE_COUNT = STARTUP_PAGE + 1;

static const char* const PAGE_NAMES[] = {
    "Appearance", "Dock", "Display", "Input", "Startup"
};
static_assert(std::size(PAGE_NAMES) == PAGE_COUNT);

static const char* const PAGE_BLURBS[] = {
    "Wallpaper and the desktop's colors",
    "Pinned launchers",
    "Each output's resolution, refresh rate and place on the desktop",
    "Keyboard repeat behavior",
    "Programs and shortcuts at session start",
};
static_assert(std::size(PAGE_BLURBS) == PAGE_COUNT);

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
            p.setBrush(palette().color(QPalette::Accent));
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

/* The Display page's arrangement, mode choices and own connection to the
 * display manager, since libstlxqpa drops layout and mode results */
struct display_state {
    stlxwin_conn* conn = nullptr;
    QSocketNotifier* notifier = nullptr;
    uint32_t generation = 0;
    uint32_t count = 0;
    stlxwin_output outputs[STLXWIN_OUTPUTS_MAX] = {};
    QPoint top_lefts[STLXWIN_OUTPUTS_MAX];
    uint32_t main_output = 0;
    uint32_t mode_watches = 0;                           /* outputs whose modes the connection asked for, by bit */
    uint32_t unchangeable_outputs = 0;                   /* outputs whose mode request was answered `-ENOTTY`, by bit */
    stlxwin_mode chosen_modes[STLXWIN_OUTPUTS_MAX] = {}; /* each output's pick, its current mode again at its MODES */
};

/* The outputs to scale, the main one under an accent strip. A dragged
 * one snaps to edges of the others within `SNAP_PX` page pixels */
class arrangement_view : public QWidget {
public:
    explicit arrangement_view(display_state* display) : m_display(display) {
        setFixedHeight(ARRANGEMENT_H);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        /* A drag holds the scale, so the pointer moves only the dragged output */
        if (m_dragged < 0) {
            scale_to_view();
        }

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        for (uint32_t i = 0; i < m_display->count; i++) {
            if (static_cast<int>(i) != m_dragged) {
                paint_output(p, i);
            }
        }

        if (m_dragged >= 0) {
            paint_output(p, static_cast<uint32_t>(m_dragged));
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        m_dragged = output_at(event->position());
        if (m_dragged < 0) {
            return;
        }

        m_grab_top_left = m_display->top_lefts[m_dragged];
        m_grab_pointer = event->position();
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_dragged < 0) {
            return;
        }

        QPoint moved = ((event->position() - m_grab_pointer) / m_scale).toPoint();
        m_display->top_lefts[m_dragged] = snapped(static_cast<uint32_t>(m_dragged), m_grab_top_left + moved);
        update();
    }

    void mouseReleaseEvent(QMouseEvent*) override {
        m_dragged = -1;
        update();
    }

private:
    QRect place_rect(uint32_t i) const {
        const stlxwin_output& output = m_display->outputs[i];
        return QRect(m_display->top_lefts[i], QSize(static_cast<int>(output.width), static_cast<int>(output.height)));
    }

    QRectF page_rect(uint32_t i) const {
        QRectF place(place_rect(i));
        return QRectF(m_origin + place.topLeft() * m_scale, place.size() * m_scale);
    }

    int output_at(QPointF position) const {
        for (uint32_t i = 0; i < m_display->count; i++) {
            if (page_rect(i).contains(position)) {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    void scale_to_view() {
        QRect bounds;
        for (uint32_t i = 0; i < m_display->count; i++) {
            bounds |= place_rect(i);
        }

        const QMarginsF margins(ARRANGEMENT_MARGIN, ARRANGEMENT_MARGIN, ARRANGEMENT_MARGIN, ARRANGEMENT_MARGIN);
        QRectF room = QRectF(rect()).marginsRemoved(margins);
        if (bounds.isEmpty() || room.isEmpty()) {
            return;
        }

        m_scale = qMin(room.width() / bounds.width(), room.height() / bounds.height());
        m_origin = room.center() - QRectF(bounds).center() * m_scale;
    }

    /* The shortest of `shift` and the four shifts that put an end of the
     * span on an end of the other */
    static int nearest_snap(int start, int length, int other_start, int other_length, int shift) {
        int end = start + length;
        int other_end = other_start + other_length;
        const int candidates[] = { other_end - start, other_start - start, other_start - end, other_end - end };
        for (int candidate : candidates) {
            if (std::abs(candidate) < std::abs(shift)) {
                shift = candidate;
            }
        }

        return shift;
    }

    /* Whether `a` and `b` touch along an edge at least one pixel long */
    static bool share_edge(QRect a, QRect b) {
        bool rows_meet = qMin(a.y() + a.height(), b.y() + b.height()) > qMax(a.y(), b.y());
        bool columns_meet = qMin(a.x() + a.width(), b.x() + b.width()) > qMax(a.x(), b.x());
        bool side_by_side = a.x() + a.width() == b.x() || b.x() + b.width() == a.x();
        bool stacked = a.y() + a.height() == b.y() || b.y() + b.height() == a.y();

        return (side_by_side && rows_meet) || (stacked && columns_meet);
    }

    /* Whether `moving` overlaps no other output and, with `dragged` there,
     * every output reaches the others through shared edges */
    bool fits(QRect moving, uint32_t dragged) const {
        QRect places[STLXWIN_OUTPUTS_MAX];
        for (uint32_t i = 0; i < m_display->count; i++) {
            places[i] = i == dragged ? moving : place_rect(i);
            if (i != dragged && moving.intersects(places[i])) {
                return false;
            }
        }

        bool reached[STLXWIN_OUTPUTS_MAX] = {};
        reached[0] = true;
        uint32_t joined = 1;
        for (bool spread = true; spread;) {
            spread = false;
            for (uint32_t i = 0; i < m_display->count; i++) {
                if (!reached[i]) {
                    continue;
                }

                for (uint32_t j = 0; j < m_display->count; j++) {
                    if (!reached[j] && share_edge(places[i], places[j])) {
                        reached[j] = true;
                        joined++;
                        spread = true;
                    }
                }
            }
        }

        return joined == m_display->count;
    }

    QPoint snapped(uint32_t dragged, QPoint top_left) const {
        int reach = static_cast<int>(SNAP_PX / m_scale);
        int shift_x = reach + 1;
        int shift_y = reach + 1;
        QRect moving(top_left, place_rect(dragged).size());
        for (uint32_t i = 0; i < m_display->count; i++) {
            if (i == dragged) {
                continue;
            }

            QRect other = place_rect(i);
            shift_x = nearest_snap(moving.x(), moving.width(), other.x(), other.width(), shift_x);
            shift_y = nearest_snap(moving.y(), moving.height(), other.y(), other.height(), shift_y);
        }

        int dx = std::abs(shift_x) <= reach ? shift_x : 0;
        int dy = std::abs(shift_y) <= reach ? shift_y : 0;
        bool x_shorter = std::abs(dx) < std::abs(dy);
        QPoint both(dx, dy);
        QPoint shorter = x_shorter ? QPoint(dx, 0) : QPoint(0, dy);
        QPoint longer = x_shorter ? QPoint(0, dy) : QPoint(dx, 0);

        /* Both snaps can leave two outputs meeting only at a corner, which
         * does not join them, so the first of these that fits applies */
        for (QPoint shift : { both, shorter, longer }) {
            if (fits(moving.translated(shift), dragged)) {
                return top_left + shift;
            }
        }

        return top_left + both;
    }

    void paint_output(QPainter& p, uint32_t i) const {
        const stlxwin_output& output = m_display->outputs[i];
        QRectF box = page_rect(i).adjusted(1, 1, -1, -1);
        bool is_dragged = static_cast<int>(i) == m_dragged;

        p.setPen(palette().color(is_dragged ? QPalette::Accent : QPalette::Midlight));
        p.setBrush(palette().color(QPalette::Button));
        p.drawRoundedRect(box, OUTPUT_RADIUS, OUTPUT_RADIUS);

        if (output.output == m_display->main_output) {
            QRectF strip = box.adjusted(OUTPUT_RADIUS, MAIN_STRIP_INSET, -OUTPUT_RADIUS, 0);
            strip.setHeight(MAIN_STRIP_PX);
            p.fillRect(strip, palette().color(QPalette::Accent));
        }

        QString label = QString::asprintf("Output %u\n%ux%u", output.output, output.width, output.height);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(box, Qt::AlignCenter, label);
    }

    display_state* m_display;
    qreal m_scale = 1;
    QPointF m_origin;
    int m_dragged = -1;
    QPoint m_grab_top_left;
    QPointF m_grab_pointer;
};

/* Everything the pages edit but the Display page's arrangement and mode
 * choices, shared by the builders and the footer */
struct app_state {
    stlxconf_t conf;
    QWidget* page_slot = nullptr;
    QWidget* page = nullptr;
    QScrollBar* page_scroll_bar = nullptr;
    QLabel* page_title = nullptr;
    QLabel* page_blurb = nullptr;
    QLabel* status = nullptr;
    nav_item* nav[PAGE_COUNT] = {};
    uint32_t active_page = 0;
    int chosen_scroll = 0; /* where the person last scrolled the page to */
    bool dirty = false;
};

/* The Display page's mode request and its trial's keep prompt, which sits
 * above the page since the trial's relight rebuilds the page */
struct trial_state {
    QWidget* bar = nullptr;
    QLabel* prompt = nullptr;
    QPushButton* keep = nullptr;
    QPushButton* set_back = nullptr;
    QTimer* countdown = nullptr;
    bool awaits_answer = false; /* a request for `mode` on `output` is sent and its answer not read */
    bool shows_prompt = false;  /* the bar shows the trial of `mode` on `output` */
    uint32_t output = 0;
    stlxwin_mode mode = {};
    uint32_t seconds_left = 0;
};

} // namespace

static app_state g_st;
static display_state g_display;
static trial_state g_trial;

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
    caption->setFont(QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont));
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

static QPushButton* add_list_button(card* parent, const char* text, std::function<void()> on_click) {
    auto* button = new QPushButton(QString::fromUtf8(text));
    QObject::connect(button, &QPushButton::clicked, std::move(on_click));

    auto* row = new QHBoxLayout;
    row->addWidget(button);
    row->addStretch(1);
    layout_of(parent)->addLayout(row);

    return button;
}

static void add_choice_row(card* parent, const char* label, QComboBox* choice) {
    QHBoxLayout* row = make_row(parent, label);
    row->addWidget(choice);
    row->addStretch(1);
}

static void build_appearance(QWidget* page) {
    card* wall = make_card(page, "WALLPAPER");
    make_text_row(wall, "Image path", g_st.conf.wallpaper, sizeof(g_st.conf.wallpaper));
    make_color_row(wall, "Fallback color", &g_st.conf.bg_color);

    card* colors = make_card(page, "BAR COLORS");
    make_color_row(colors, "Bar", &g_st.conf.bar_color);
    make_color_row(colors, "Text", &g_st.conf.text_color);
    make_number_row(colors, "Bar font size", &g_st.conf.bar_font_size);
}

static void build_dock(QWidget* page) {
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

static const char* layout_outcome_text(uint32_t outcome) {
    switch (outcome) {
    case STLXWIN_LAYOUT_APPLIED:
        return "layout applied";
    case STLXWIN_LAYOUT_UNSAVED:
        return "layout applied but not stored, so it is lost when the display manager exits";
    case STLXWIN_LAYOUT_STALE:
        return "refused, the outputs changed meanwhile, arrange them again";
    case STLXWIN_LAYOUT_INVALID:
        return "refused as invalid";
    case STLXWIN_LAYOUT_OVERLAP:
        return "refused, two outputs overlap";
    case STLXWIN_LAYOUT_APART:
        return "refused, an output does not connect to the main output";
    default:
        return "refused";
    }
}

static bool sizes_equal(const stlxwin_mode& a, const stlxwin_mode& b) {
    return a.width == b.width && a.height == b.height;
}

static bool modes_equal(const stlxwin_mode& a, const stlxwin_mode& b) {
    return sizes_equal(a, b) && a.refresh_millihertz == b.refresh_millihertz;
}

/* What the display manager did, for an answer whose `result` is 0 */
static const char* mode_outcome_text(uint32_t outcome) {
    switch (outcome) {
    case STLXWIN_MODE_SET:
        return "display mode set";
    case STLXWIN_MODE_TRIAL:
        return "display mode on trial";
    case STLXWIN_MODE_KEPT:
        return "display mode kept";
    case STLXWIN_MODE_SET_BACK:
        return "display mode set back";
    case STLXWIN_MODE_DEFAULT:
        return "setting the display mode back failed, so the output's default was set";
    case STLXWIN_MODE_NO_BUFFERS:
        return "display mode set back, the desktop has no buffers at that size";
    case STLXWIN_MODE_BUSY:
        return "refused, a trial is running";
    case STLXWIN_MODE_NO_TRIAL:
        return "refused, the trial had already ended";
    case STLXWIN_MODE_INVALID:
        return "refused as invalid";
    default:
        return "answered";
    }
}

/* What did not happen when the display refused the mode the display manager asked for last */
static const char* mode_failure_text(uint32_t outcome) {
    switch (outcome) {
    case STLXWIN_MODE_SET:
        return "display mode not set";
    case STLXWIN_MODE_TRIAL:
        return "display mode not set, so no trial started";
    case STLXWIN_MODE_SET_BACK:
        return "display mode not set back";
    case STLXWIN_MODE_DEFAULT:
        return "display mode not set back, nor set to the output's default";
    case STLXWIN_MODE_NO_BUFFERS:
        return "the desktop has no buffers at that size, and setting the display mode back failed";
    default:
        return "the display refused a display mode";
    }
}

static QString mode_result_text(uint32_t outcome, int32_t result) {
    if (result == 0) {
        return QString::fromUtf8(mode_outcome_text(outcome));
    }

    if (result == -ENOTTY) {
        return QStringLiteral("display mode not set, this display cannot change modes");
    }

    return QString::asprintf("%s, the display answered: %s", mode_failure_text(outcome), strerror(-result));
}

/* Whether an outcome answers a mode request. The page sends no keep
 * that `STLXWIN_MODE_INVALID` would answer */
static bool answers_request(uint32_t outcome) {
    switch (outcome) {
    case STLXWIN_MODE_SET:
    case STLXWIN_MODE_TRIAL:
    case STLXWIN_MODE_NO_BUFFERS:
    case STLXWIN_MODE_BUSY:
    case STLXWIN_MODE_INVALID:
        return true;
    default:
        return false;
    }
}

/* Counted from the trial's answer, so 0 comes just after the display manager's own timer ends */
static void show_countdown() {
    if (g_trial.seconds_left == 0) {
        g_trial.prompt->setText(QStringLiteral("Setting the display mode back"));
        return;
    }

    const char* unit = g_trial.seconds_left == 1 ? "second" : "seconds";
    QString text = QString::asprintf("Keep this display mode? Setting it back in %u %s", g_trial.seconds_left, unit);
    g_trial.prompt->setText(text);
}

static void start_keep_prompt() {
    g_trial.shows_prompt = true;
    g_trial.seconds_left = TRIAL_SECONDS;

    show_countdown();
    g_trial.keep->setEnabled(true);
    g_trial.set_back->setEnabled(true);
    g_trial.bar->show();
    g_trial.countdown->start(COUNTDOWN_TICK_MS);
}

static void end_keep_prompt() {
    g_trial.shows_prompt = false;
    g_trial.countdown->stop();
    g_trial.bar->hide();
}

static void follow_modes(uint32_t output) {
    stlxwin_modes_info info;
    if (stlxwin_modes_get(g_display.conn, output, &info, nullptr, 0) < 0) {
        return;
    }

    g_display.chosen_modes[output] = info.current;

    uint32_t output_count = qMin(info.output_count, STLXWIN_OUTPUTS_MAX);
    for (uint32_t i = 0; i < output_count; i++) {
        bool is_watched = (g_display.mode_watches & (1u << i)) != 0;
        if (!is_watched && stlxwin_modes_watch(g_display.conn, i) == 0) {
            g_display.mode_watches |= 1u << i;
        }
    }

    /* Leaving the trial's mode ends the prompt: a set-back's relight comes
     * before its answer, and a trial that an output event ends gets none */
    bool is_trial_output = g_trial.shows_prompt && output == g_trial.output;
    if (is_trial_output && !modes_equal(info.current, g_trial.mode)) {
        end_keep_prompt();
        set_status("the display mode changed and the trial ended");
    }
}

static void follow_mode_result(const stlxwin_event& event) {
    uint32_t output = event.mode_result.output;
    uint32_t outcome = event.mode_result.outcome;
    int32_t result = event.mode_result.result;

    set_status(mode_result_text(outcome, result).toUtf8().constData());

    /* The display manager can answer a set-back after its relight ended the
     * prompt, so only a request's own answers clear `awaits_answer` */
    if (answers_request(outcome)) {
        g_trial.awaits_answer = false;
    }

    if (result == -ENOTTY && output < STLXWIN_OUTPUTS_MAX) {
        g_display.unchangeable_outputs |= 1u << output;
    }

    bool is_trial_output = g_trial.shows_prompt && output == g_trial.output;
    if (outcome == STLXWIN_MODE_TRIAL && result == 0) {
        start_keep_prompt();
    } else if (is_trial_output && !answers_request(outcome)) {
        end_keep_prompt();
    }
}

static void close_display_connection() {
    g_display.notifier->setEnabled(false);
    g_display.notifier->deleteLater();
    g_display.notifier = nullptr;

    stlxwin_disconnect(g_display.conn);
    g_display.conn = nullptr;
    g_display.mode_watches = 0;
    g_display.unchangeable_outputs = 0;

    g_trial.awaits_answer = false;
    end_keep_prompt();
}

/* While the Display page shows, each `STLXWIN_EVT_OUTPUTS`, mode event or
 * lost connection rebuilds it. The footer shows layout and mode results */
static void read_display_events() {
    bool alive = stlxwin_dispatch(g_display.conn) >= 0;
    bool outputs_changed = false;
    bool mode_event_read = false;

    stlxwin_event event;
    while (alive && stlxwin_next_event(g_display.conn, &event)) {
        if (event.type == STLXWIN_EVT_DISCONNECTED) {
            alive = false;
        } else if (event.type == STLXWIN_EVT_OUTPUTS) {
            outputs_changed = true;
        } else if (event.type == STLXWIN_EVT_LAYOUT_RESULT) {
            set_status(layout_outcome_text(event.layout_result.outcome));
        } else if (event.type == STLXWIN_EVT_MODES) {
            follow_modes(event.modes.output);
            mode_event_read = true;
        } else if (event.type == STLXWIN_EVT_MODE_RESULT) {
            follow_mode_result(event);
            mode_event_read = true;
        }
    }

    if (!alive) {
        close_display_connection();
    }

    bool page_stale = outputs_changed || mode_event_read || !alive;
    bool page_shown = g_st.active_page == DISPLAY_PAGE;
    if (page_stale && page_shown) {
        rebuild_page_later();
    }
}

static bool open_display_connection() {
    if (g_display.conn) {
        return true;
    }

    g_display.conn = stlxwin_connect("stlxsettings");
    if (!g_display.conn) {
        return false;
    }

    /* Output 0's MODES counts the outputs, which `follow_modes` then watches */
    if (stlxwin_modes_watch(g_display.conn, 0) == 0) {
        g_display.mode_watches = 1;
    }

    g_display.notifier = new QSocketNotifier(stlxwin_conn_fd(g_display.conn), QSocketNotifier::Read);
    QObject::connect(g_display.notifier, &QSocketNotifier::activated, read_display_events);

    return true;
}

static QString output_name(const stlxwin_output& output) {
    QString name = QStringLiteral("Output %1").arg(output.output);
    size_t monitor_len = strnlen(output.monitor, sizeof(output.monitor));
    if (monitor_len > 0) {
        name += QStringLiteral(" (%1)").arg(QString::fromUtf8(output.monitor, static_cast<qsizetype>(monitor_len)));
    }

    return name;
}

/* The places and main output the display manager lists, the first output by default */
static void read_arrangement(uint32_t generation) {
    g_display.generation = generation;
    g_display.main_output = g_display.outputs[0].output;
    for (uint32_t i = 0; i < g_display.count; i++) {
        const stlxwin_output& output = g_display.outputs[i];
        g_display.top_lefts[i] = QPoint(output.x, output.y);
        if (output.flags & STLXWIN_DESKTOP_MAIN) {
            g_display.main_output = output.output;
        }
    }
}

/* A sent request's outcome arrives as `STLXWIN_EVT_LAYOUT_RESULT`,
 * which `read_display_events` shows in the footer */
static void apply_layout() {
    stlxwin_layout_entry entries[STLXWIN_OUTPUTS_MAX];
    for (uint32_t i = 0; i < g_display.count; i++) {
        entries[i] = { g_display.outputs[i].output, g_display.top_lefts[i].x(), g_display.top_lefts[i].y() };
    }

    int rc = stlxwin_layout_set(
        g_display.conn,
        g_display.generation,
        g_display.main_output,
        entries,
        g_display.count
    );
    if (rc != 0) {
        set_status("the layout could not be sent to the display manager");
    }
}

/* The output's name as the desktop lists it, or its number when the desktop does not place it */
static QString output_title(uint32_t output) {
    for (uint32_t i = 0; i < g_display.count; i++) {
        if (g_display.outputs[i].output == output) {
            return output_name(g_display.outputs[i]);
        }
    }

    stlxwin_output unplaced = {};
    unplaced.output = output;

    return output_name(unplaced);
}

static bool can_apply_mode(uint32_t output, const stlxwin_mode& current) {
    bool is_idle = !g_trial.awaits_answer && !g_trial.shows_prompt;
    return is_idle && !modes_equal(g_display.chosen_modes[output], current);
}

/* The answer arrives as `STLXWIN_EVT_MODE_RESULT`, which `follow_mode_result` reads */
static void apply_mode(uint32_t output) {
    stlxwin_modes_info info;
    if (stlxwin_modes_get(g_display.conn, output, &info, nullptr, 0) < 0) {
        return;
    }

    /* Clicks can arrive before the posted rebuild greys Apply out, after an Apply or a MODES */
    if (!can_apply_mode(output, info.current)) {
        return;
    }

    const stlxwin_mode& chosen = g_display.chosen_modes[output];
    if (stlxwin_mode_set(g_display.conn, output, &chosen, STLXWIN_SET_MODE_TRIAL) != 0) {
        set_status("the display mode could not be sent to the display manager");
        return;
    }

    g_trial.awaits_answer = true;
    g_trial.output = output;
    g_trial.mode = chosen;
    rebuild_page_later();
}

static QString rate_text(uint64_t refresh_millihertz) {
    if (refresh_millihertz == 0) {
        return QStringLiteral("unknown rate");
    }

    return QString::asprintf(
        "%llu.%03llu Hz",
        static_cast<unsigned long long>(refresh_millihertz / MILLIHERTZ_PER_HERTZ),
        static_cast<unsigned long long>(refresh_millihertz % MILLIHERTZ_PER_HERTZ)
    );
}

static bool is_first_of_size(const std::vector<stlxwin_mode>& listed, size_t index) {
    for (size_t i = 0; i < index; i++) {
        if (sizes_equal(listed[i], listed[index])) {
            return false;
        }
    }

    return true;
}

static bool is_listed(const std::vector<stlxwin_mode>& listed, const stlxwin_mode& mode) {
    for (const stlxwin_mode& entry : listed) {
        if (modes_equal(entry, mode)) {
            return true;
        }
    }

    return false;
}

static void fill_resolution_choice(QComboBox* choice, const std::vector<stlxwin_mode>& listed,
                                   const stlxwin_mode& chosen) {
    for (size_t i = 0; i < listed.size(); i++) {
        if (!is_first_of_size(listed, i)) {
            continue;
        }

        QString text = QString::asprintf("%ux%u", listed[i].width, listed[i].height);
        if (i == 0) {
            text += QStringLiteral(" (default)");
        }

        choice->addItem(text, static_cast<int>(i));
        if (sizes_equal(listed[i], chosen)) {
            choice->setCurrentIndex(choice->count() - 1);
        }
    }
}

static void fill_refresh_choice(QComboBox* choice, const std::vector<stlxwin_mode>& listed,
                                const stlxwin_mode& chosen) {
    QSignalBlocker blocker(choice);
    choice->clear();

    for (size_t i = 0; i < listed.size(); i++) {
        if (!sizes_equal(listed[i], chosen)) {
            continue;
        }

        QString text = rate_text(listed[i].refresh_millihertz);
        if (i == 0) {
            text += QStringLiteral(" (default)");
        }

        choice->addItem(text, static_cast<int>(i));
        if (listed[i].refresh_millihertz == chosen.refresh_millihertz) {
            choice->setCurrentIndex(choice->count() - 1);
        }
    }
}

static void build_mode_card(QWidget* page, uint32_t output) {
    stlxwin_modes_info info;
    stlxwin_mode modes[STLXWIN_MODES_MAX];
    long count = stlxwin_modes_get(g_display.conn, output, &info, modes, STLXWIN_MODES_MAX);
    if (count <= 0 || (info.flags & STLXWIN_OUTPUT_LIT) == 0) {
        return;
    }

    /* A choice made from a list that a later MODES replaced may not be listed any more */
    std::vector<stlxwin_mode> listed(modes, modes + count);
    stlxwin_mode current = info.current;
    if (!is_listed(listed, g_display.chosen_modes[output])) {
        g_display.chosen_modes[output] = current;
    }

    auto* resolution = new QComboBox;
    auto* refresh = new QComboBox;
    fill_resolution_choice(resolution, listed, g_display.chosen_modes[output]);
    fill_refresh_choice(refresh, listed, g_display.chosen_modes[output]);

    bool is_greyed = count == 1 || (g_display.unchangeable_outputs & (1u << output)) != 0;
    resolution->setEnabled(!is_greyed);
    refresh->setEnabled(!is_greyed);

    card* choices = make_card(page, output_title(output).toUpper().toUtf8().constData());
    add_choice_row(choices, "Resolution", resolution);
    add_choice_row(choices, "Refresh rate", refresh);
    QPushButton* apply = add_list_button(choices, "Apply", [output] { apply_mode(output); });
    apply->setEnabled(!is_greyed && can_apply_mode(output, current));

    QObject::connect(resolution, &QComboBox::currentIndexChanged, [=](int index) {
        g_display.chosen_modes[output] = listed[resolution->itemData(index).toUInt()];
        fill_refresh_choice(refresh, listed, g_display.chosen_modes[output]);
        apply->setEnabled(can_apply_mode(output, current));
    });

    QObject::connect(refresh, &QComboBox::currentIndexChanged, [=](int index) {
        g_display.chosen_modes[output] = listed[refresh->itemData(index).toUInt()];
        apply->setEnabled(can_apply_mode(output, current));
    });
}

static void build_display(QWidget* page) {
    card* arrangement = make_card(page, "ARRANGEMENT");

    bool fresh_connection = !g_display.conn;
    uint32_t generation = 0;
    long listed = -1;
    if (open_display_connection()) {
        listed = stlxwin_outputs_get(g_display.conn, &generation, g_display.outputs, STLXWIN_OUTPUTS_MAX);
    }

    g_display.count = static_cast<uint32_t>(qBound(0L, listed, static_cast<long>(STLXWIN_OUTPUTS_MAX)));
    if (g_display.count == 0) {
        auto* none = new QLabel(listed < 0 ? QStringLiteral("The display manager could not be reached")
                                           : QStringLiteral("The display manager lists no outputs to arrange"));
        none->setContentsMargins(4, 0, 0, 0);
        set_dim(none);
        layout_of(arrangement)->addWidget(none);
        return;
    }

    /* Moves not applied last until the list or the connection changes,
     * so the rebuilds of Save, Revert and a page switch keep them */
    if (fresh_connection || generation != g_display.generation) {
        read_arrangement(generation);
    }

    auto* view = new arrangement_view(&g_display);
    layout_of(arrangement)->addWidget(view);

    auto* main_choice = new QComboBox;
    for (uint32_t i = 0; i < g_display.count; i++) {
        main_choice->addItem(output_name(g_display.outputs[i]));
        if (g_display.outputs[i].output == g_display.main_output) {
            main_choice->setCurrentIndex(static_cast<int>(i));
        }
    }

    add_choice_row(arrangement, "Main output", main_choice);
    QObject::connect(main_choice, &QComboBox::currentIndexChanged, [view](int index) {
        if (index >= 0) {
            g_display.main_output = g_display.outputs[index].output;
            view->update();
        }
    });

    add_list_button(arrangement, "Apply", apply_layout);

    for (uint32_t output = 0; output < STLXWIN_OUTPUTS_MAX; output++) {
        build_mode_card(page, output);
    }
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
    if (index != g_st.active_page) {
        g_st.chosen_scroll = 0;
        g_st.page_scroll_bar->setValue(0);
    }

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

    switch (static_cast<page_index>(index)) {
    case APPEARANCE_PAGE: build_appearance(g_st.page); break;
    case DOCK_PAGE: build_dock(g_st.page); break;
    case DISPLAY_PAGE: build_display(g_st.page); break;
    case INPUT_PAGE: build_input(g_st.page); break;
    case STARTUP_PAGE: build_startup(g_st.page); break;
    }

    page_layout->addStretch(1);
    layout_of(g_st.page_slot)->addWidget(g_st.page);
}

/* A rebuild can empty the scroll bar's range for a moment, which puts the
 * page at its top, so each range change sets the person's last scroll */
static void track_page_scroll(QScrollBar* page_scroll_bar) {
    g_st.page_scroll_bar = page_scroll_bar;
    QObject::connect(page_scroll_bar, &QAbstractSlider::actionTriggered, [page_scroll_bar] {
        g_st.chosen_scroll = page_scroll_bar->sliderPosition();
    });

    QObject::connect(page_scroll_bar, &QAbstractSlider::rangeChanged, [page_scroll_bar] {
        page_scroll_bar->setValue(g_st.chosen_scroll);
    });
}

/* Clamp the fields a broken save could take the desktop down with */
static void sanitize_conf() {
    stlxconf_t& c = g_st.conf;

    if (c.bar_font_size < 8) c.bar_font_size = 8;
    if (c.bar_font_size > 48) c.bar_font_size = 48;
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
    palette.setColor(QPalette::Window, palette.color(QPalette::Dark));
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
    version->setFont(QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont));
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
    g_st.page_blurb->setFont(QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont));
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
    g_st.status->setFont(QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont));
    set_dim(g_st.status);
    layout->addWidget(g_st.status);
    layout->addStretch(1);

    auto* revert = new QPushButton(QStringLiteral("Revert"));
    QObject::connect(revert, &QPushButton::clicked, revert_conf);
    layout->addWidget(revert);

    /* The default button takes the accent, which marks the action that writes the file */
    auto* save = new QPushButton(QStringLiteral("Save"));
    save->setDefault(true);
    QObject::connect(save, &QPushButton::clicked, save_conf);
    layout->addWidget(save);

    return footer;
}

/* The prompt stays until the display manager's answer, the output's change
 * of mode or a lost connection shows that the trial ended */
static void answer_trial(bool keep) {
    g_trial.keep->setEnabled(false);
    g_trial.set_back->setEnabled(false);

    if (!keep) {
        g_trial.countdown->stop();
        g_trial.seconds_left = 0;
        show_countdown();
    }

    if (stlxwin_mode_keep(g_display.conn, g_trial.output, keep ? 1 : 0) != 0) {
        set_status("the answer could not be sent to the display manager");
    }
}

/* At 0 the display manager is setting the mode back, so the prompt takes no answer */
static void count_trial_down() {
    g_trial.seconds_left--;
    if (g_trial.seconds_left == 0) {
        g_trial.countdown->stop();
        g_trial.keep->setEnabled(false);
        g_trial.set_back->setEnabled(false);
    }

    show_countdown();
}

static QWidget* make_keep_bar() {
    auto* bar = new QWidget;
    auto* margins = new QVBoxLayout(bar);
    margins->setContentsMargins(20, 4, 20, 4);

    auto* panel = new card;
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    layout_of(panel)->addLayout(row);
    margins->addWidget(panel);

    g_trial.prompt = new QLabel;
    row->addWidget(g_trial.prompt);
    row->addStretch(1);

    g_trial.set_back = new QPushButton(QStringLiteral("Set back"));
    QObject::connect(g_trial.set_back, &QPushButton::clicked, [] { answer_trial(false); });
    row->addWidget(g_trial.set_back);

    g_trial.keep = new QPushButton(QStringLiteral("Keep"));
    QObject::connect(g_trial.keep, &QPushButton::clicked, [] { answer_trial(true); });
    row->addWidget(g_trial.keep);

    /* Coarse ticks can come early and end the count before the display manager's timer */
    g_trial.countdown = new QTimer(bar);
    g_trial.countdown->setTimerType(Qt::PreciseTimer);
    QObject::connect(g_trial.countdown, &QTimer::timeout, count_trial_down);

    g_trial.bar = bar;
    bar->hide();

    return bar;
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

    /* The content column: header, a trial's keep prompt, the scrolling page, footer */
    auto* content = new QVBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    content->addWidget(make_header());
    content->addWidget(make_keep_bar());

    g_st.page_slot = new QWidget;
    auto* slot_layout = new QVBoxLayout(g_st.page_slot);
    slot_layout->setContentsMargins(20, 4, 20, 16);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(g_st.page_slot);
    track_page_scroll(scroll->verticalScrollBar());

    content->addWidget(scroll, 1);
    content->addWidget(make_footer());
    root->addLayout(content, 1);

    build_page(APPEARANCE_PAGE);
    window.show();

    return app.exec();
}
