/* stlxsettings: the desktop configuration editor. Setting rows show the config key each writes. Save writes the
 * file and signals the display manager to re-read it, while the Displays page's Apply sends its own requests. */
#include "kit.hpp"
#include "settings.hpp"

#include <stlxconf/conf.h>
#include <stlxtheme/theme.h>
#include <stlxwin/stlxwin.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSocketNotifier>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <sys/utsname.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <vector>

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
constexpr int MENU_BUTTON_W = 36;
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

/* A trial's keep prompt: a card above the page, the seconds left in a ring that empties with them */
constexpr int PROMPT_GAP = 12;
constexpr int PROMPT_PADDING_X = 12;
constexpr int PROMPT_PADDING_Y = 8;
constexpr int RING_PX = 26;
constexpr int RING_STROKE = 2;
constexpr int FULL_TURN = 360 * 16; /* QPainter arcs count sixteenths of a degree */
constexpr int RING_START = 90 * 16; /* the ring empties from its top */

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

class countdown_ring : public QWidget {
public:
    countdown_ring() {
        setFixedSize(RING_PX, RING_PX);
    }

    void show_seconds(uint32_t seconds_left) {
        m_seconds_left = seconds_left;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        qreal half_stroke = RING_STROKE / 2.0;
        QRectF ring = QRectF(rect()).adjusted(half_stroke, half_stroke, -half_stroke, -half_stroke);

        painter.setPen(QPen(theme_color(STLXTHEME_LINE), RING_STROKE));
        painter.drawEllipse(ring);
        painter.setPen(QPen(theme_color(STLXTHEME_TEXT), RING_STROKE));
        painter.drawArc(ring, RING_START, -static_cast<int>(FULL_TURN * m_seconds_left / TRIAL_SECONDS));

        painter.setFont(mono_font(STLXTHEME_CAPTION_SIZE));
        painter.drawText(rect(), Qt::AlignCenter, QString::number(m_seconds_left));
    }

private:
    uint32_t m_seconds_left = 0;
};

/* The Displays page's arrangement, mode choices and own connection to the
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

/* The Displays page's mode request and its trial's keep prompt. The prompt sits above the page, which a trial's
 * relight rebuilds, so the rebuild does not delete it and it stays answerable from every page */
struct trial_state {
    QWidget* prompt = nullptr;
    countdown_ring* ring = nullptr;
    QLabel* question = nullptr;
    QLabel* time_left = nullptr;
    push_button* keep = nullptr;
    push_button* set_back = nullptr;
    QTimer* countdown = nullptr;
    bool awaits_answer = false; /* a request for `mode` on `output` is sent and its answer not read */
    bool shows_prompt = false;  /* the prompt shows the trial of `mode` on `output` */
    uint32_t output = 0;
    stlxwin_mode mode = {};
    stlxwin_mode previous = {}; /* the output's mode when the request went out, which a set back returns to */
    uint32_t seconds_left = 0;
};

} // namespace

static app_state g_st;
static display_state g_display;
static trial_state g_trial;

static void build_page(uint32_t index);

static void set_dim(QWidget* widget) {
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, theme_color(STLXTHEME_TEXT_DIM));
    widget->setPalette(palette);
}

/* The footer shows each message as a sentence, whatever case its text starts in */
static void set_status(const QString& text) {
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
    QString path = QString::fromUtf8(autostart.path);
    return path.isEmpty() ? QStringLiteral("A program") : path.section(QLatin1Char('/'), -1);
}

template <typename item_t>
static int find_entry(const item_t* items, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(items[i].name, name) == 0) {
            return static_cast<int>(i);
        }
    }

    return -1;
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

/* What Save would write: one description for each changed setting, list entry and list order */
static QStringList pending_changes() {
    const stlxconf_t& saved = g_st.saved;
    const stlxconf_t& edited = g_conf;
    QStringList changes;

    note_change(strcmp(saved.wallpaper, edited.wallpaper) != 0, QStringLiteral("Wallpaper changed"), changes);
    note_change(saved.glass_level != edited.glass_level, QStringLiteral("Glass changed"), changes);
    note_change(saved.glass_tint != edited.glass_tint, QStringLiteral("Tint changed"), changes);

    bool is_delay_changed = saved.key_repeat_delay_ms != edited.key_repeat_delay_ms;
    bool is_interval_changed = saved.key_repeat_interval_ms != edited.key_repeat_interval_ms;
    note_change(is_delay_changed, QStringLiteral("Repeat delay changed"), changes);
    note_change(is_interval_changed, QStringLiteral("Repeat interval changed"), changes);

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

/* Pages rebuild once the event that asked for it is done, since its widget goes away */
static void rebuild_page_later() {
    QTimer::singleShot(0, [] { build_page(g_st.active_page); });
}

void copy_string(char* destination, size_t capacity, const QString& text) {
    QByteArray utf8 = text.toUtf8();
    size_t length = qMin(static_cast<size_t>(utf8.size()), capacity - 1);
    memcpy(destination, utf8.constData(), length);
    destination[length] = '\0';
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

/* `prefix` followed by the lowest number that gives a name no entry of the list holds */
template <typename item_t>
static QString unused_entry_name(const char* prefix, const item_t* items, uint32_t count) {
    uint32_t n = 0;
    while (true) {
        QByteArray name = QStringLiteral("%1%2").arg(QLatin1String(prefix)).arg(n).toUtf8();
        if (find_entry(items, count, name.constData()) < 0) {
            return QString::fromUtf8(name);
        }

        n++;
    }
}

static uint32_t parse_u32(const QString& s, int base) {
    return static_cast<uint32_t>(strtoul(s.toUtf8().constData(), nullptr, base));
}

static QVBoxLayout* layout_of(QWidget* widget) {
    return static_cast<QVBoxLayout*>(widget->layout());
}

static QLabel* make_caption(const QString& text) {
    return make_text(text, mono_font(STLXTHEME_CAPTION_SIZE), STLXTHEME_TEXT_DIM);
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

/* A bare row for list entries, no label column */
static QHBoxLayout* make_list_row(card* parent) {
    auto* row = new QWidget;
    row->setMinimumHeight(LIST_ROW_H);
    parent->add_row(row);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(LIST_ROW_PADDING_X, 0, LIST_ROW_PADDING_X, 0);
    layout->setSpacing(FOOTER_GAP);

    return layout;
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
        copy_string(dst, cap, s);
        show_pending_changes();
    });

    return field;
}

/* A short numeric field writing through to a config integer */
static void make_number_row(card* parent, const char* label, const char* key, uint32_t* value) {
    QHBoxLayout* row = make_row(parent, label, key);

    QLineEdit* field = make_field(QString::number(*value));
    field->setFixedWidth(80);
    row->addWidget(field);

    QObject::connect(field, &QLineEdit::textEdited, [value](const QString& s) {
        *value = parse_u32(s, 10);
        show_pending_changes();
    });
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

        show_pending_changes();
        rebuild_page_later();
    });
}

static push_button* add_list_button(card* parent, const char* text, std::function<void()> on_click) {
    auto* button = new push_button(QString::fromUtf8(text));
    button->set_small(true);
    button->on_click = std::move(on_click);

    QHBoxLayout* row = make_list_row(parent);
    row->addWidget(button);
    row->addStretch(1);

    return button;
}

static void add_choice_row(card* parent, const char* label, const char* key, QComboBox* choice) {
    QHBoxLayout* row = make_row(parent, label, key);
    row->addWidget(choice);
    row->addStretch(1);
}

static void build_dock(QWidget* page) {
    card* pins = add_section(page, "Pinned apps", "[taskbar:<id>]  label  path  args  icon");

    auto move_pin = [](uint32_t from, uint32_t to) {
        stlxconf_pin_t tmp = g_conf.pins[from];
        g_conf.pins[from] = g_conf.pins[to];
        g_conf.pins[to] = tmp;
    };
    auto remove_pin = [](uint32_t at) {
        for (uint32_t i = at; i + 1 < g_conf.pin_count; i++) {
            g_conf.pins[i] = g_conf.pins[i + 1];
        }

        g_conf.pin_count--;
    };

    QHBoxLayout* header = make_list_row(pins);
    add_caption(header, "LABEL", 110);
    add_caption(header, "PROGRAM", 0);
    add_caption(header, "ARGUMENTS", 130);
    add_caption(header, "ICON", 0);
    header->addSpacing(MENU_BUTTON_W + 20);

    for (uint32_t i = 0; i < g_conf.pin_count; i++) {
        stlxconf_pin_t* pin = &g_conf.pins[i];

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
        make_row_menu(row, i, owned_count(g_conf.pins, g_conf.pin_count), move_pin, remove_pin);
    }

    add_list_button(pins, "Add app", [] {
        if (g_conf.pin_count >= STLXCONF_MAX_TASKBAR) {
            set_status("the dock is full");
            return;
        }

        uint32_t owned = owned_count(g_conf.pins, g_conf.pin_count);
        QString name = unused_entry_name("pin", g_conf.pins, g_conf.pin_count);
        stlxconf_pin_t* pin = insert_slot(g_conf.pins, g_conf.pin_count, owned);
        copy_string(pin->name, sizeof(pin->name), name);
        copy_string(pin->label, sizeof(pin->label), QStringLiteral("New app"));
        g_conf.pin_count++;

        show_pending_changes();
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

static QString size_text(const stlxwin_mode& mode) {
    return QString::asprintf("%ux%u", mode.width, mode.height);
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
    g_trial.ring->show_seconds(g_trial.seconds_left);
    if (g_trial.seconds_left == 0) {
        g_trial.time_left->setText(QStringLiteral("Setting the display mode back"));
        return;
    }

    const stlxwin_mode& previous = g_trial.previous;
    QString shown = QStringLiteral("%1 at %2").arg(size_text(previous), rate_text(previous.refresh_millihertz));
    const char* unit = g_trial.seconds_left == 1 ? "second" : "seconds";
    g_trial.time_left->setText(
        QStringLiteral("Setting it back to %1 in %2 %3").arg(shown).arg(g_trial.seconds_left).arg(QLatin1String(unit))
    );
}

static void start_keep_prompt() {
    g_trial.shows_prompt = true;
    g_trial.seconds_left = TRIAL_SECONDS;

    show_countdown();
    g_trial.keep->setEnabled(true);
    g_trial.set_back->setEnabled(true);
    g_trial.prompt->show();
    g_trial.countdown->start(COUNTDOWN_TICK_MS);
}

static void end_keep_prompt() {
    g_trial.shows_prompt = false;
    g_trial.countdown->stop();
    g_trial.prompt->hide();
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

    set_status(mode_result_text(outcome, result));

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

/* While the Displays page shows, each `STLXWIN_EVT_OUTPUTS`, mode event or
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
    g_trial.previous = info.current;

    QString shown = QStringLiteral("%1 at %2").arg(size_text(chosen), rate_text(chosen.refresh_millihertz));
    g_trial.question->setText(QStringLiteral("Keep %1 on %2?").arg(shown, output_title(output)));
    rebuild_page_later();
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

    card* choices = add_section(page, output_title(output), QString());
    add_choice_row(choices, "Resolution", "", resolution);
    add_choice_row(choices, "Refresh rate", "", refresh);
    push_button* apply = add_list_button(choices, "Apply", [output] { apply_mode(output); });
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
    card* arrangement = add_section(page, "Arrangement", "[layout]");

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
        set_dim(none);
        make_list_row(arrangement)->addWidget(none);
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

    add_choice_row(arrangement, "Main output", "main", main_choice);
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
    card* repeat = add_section(page, "Keyboard", "[input]");

    auto* enabled = new QCheckBox;
    enabled->setChecked(g_conf.key_repeat_delay_ms != 0);
    make_row(repeat, "Repeat held keys", "key_repeat_delay_ms > 0")->addWidget(enabled);
    QObject::connect(enabled, &QCheckBox::toggled, [](bool on) {
        g_conf.key_repeat_delay_ms = on ? REPEAT_DELAY_MS : 0;
        show_pending_changes();
        rebuild_page_later();
    });

    if (g_conf.key_repeat_delay_ms != 0) {
        make_number_row(repeat, "Delay (ms)", "key_repeat_delay_ms", &g_conf.key_repeat_delay_ms);
        make_number_row(repeat, "Interval (ms)", "key_repeat_interval_ms", &g_conf.key_repeat_interval_ms);
    }
}

static void build_startup(QWidget* page) {
    card* autos = add_section(page, "Autostart", "[autostart:<id>]  path  args");

    auto move_auto = [](uint32_t from, uint32_t to) {
        stlxconf_autostart_t tmp = g_conf.autostart[from];
        g_conf.autostart[from] = g_conf.autostart[to];
        g_conf.autostart[to] = tmp;
    };
    auto remove_auto = [](uint32_t at) {
        for (uint32_t i = at; i + 1 < g_conf.autostart_count; i++) {
            g_conf.autostart[i] = g_conf.autostart[i + 1];
        }

        g_conf.autostart_count--;
    };

    QHBoxLayout* header = make_list_row(autos);
    add_caption(header, "PROGRAM", 0);
    add_caption(header, "ARGUMENTS", 130);
    header->addSpacing(MENU_BUTTON_W + 20);

    for (uint32_t i = 0; i < g_conf.autostart_count; i++) {
        stlxconf_autostart_t* as = &g_conf.autostart[i];

        QHBoxLayout* row = make_list_row(autos);
        if (as->from_drop_in) {
            add_value(row, as->path, 0);
            add_value(row, as->args, 130);
            mark_drop_in_row(row);
            continue;
        }

        add_text_field(row, as->path, sizeof(as->path), 0);
        add_text_field(row, as->args, sizeof(as->args), 130);
        make_row_menu(row, i, owned_count(g_conf.autostart, g_conf.autostart_count), move_auto,
                      remove_auto);
    }

    add_list_button(autos, "Add program", [] {
        if (g_conf.autostart_count >= STLXCONF_MAX_AUTOSTART) {
            set_status("autostart table is full");
            return;
        }

        uint32_t owned = owned_count(g_conf.autostart, g_conf.autostart_count);
        QString name = unused_entry_name("entry", g_conf.autostart, g_conf.autostart_count);
        stlxconf_autostart_t* as = insert_slot(g_conf.autostart, g_conf.autostart_count, owned);
        copy_string(as->name, sizeof(as->name), name);
        g_conf.autostart_count++;

        show_pending_changes();
        rebuild_page_later();
    });

    card* keys = add_section(page, "Shortcuts", "[shortcut:<id>]  key  action  path  args");

    QHBoxLayout* kh = make_list_row(keys);
    add_caption(kh, "CHORD", 130);
    add_caption(kh, "PROGRAM", 0);

    for (uint32_t i = 0; i < g_conf.shortcut_count; i++) {
        stlxconf_shortcut_t* sc = &g_conf.shortcuts[i];

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
    QObject::connect(page_scroll_bar, &QAbstractSlider::actionTriggered, [page_scroll_bar] {
        g_st.chosen_scroll = page_scroll_bar->sliderPosition();
    });

    QObject::connect(page_scroll_bar, &QAbstractSlider::rangeChanged, [page_scroll_bar] {
        page_scroll_bar->setValue(g_st.chosen_scroll);
    });
}

/* Clamp the fields a broken save could take the desktop down with */
static void sanitize_conf() {
    stlxconf_t& c = g_conf;

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
    stlxconf_load(&g_conf, STLXCONF_PATH);
    stlxconf_load_drop_ins(&g_conf, STLXCONF_DROP_IN_DIR);
    g_st.saved = g_conf;
}

static void save_conf() {
    sanitize_conf();

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

static QWidget* make_keep_prompt() {
    auto* prompt = new QWidget;
    auto* margins = new QVBoxLayout(prompt);
    margins->setContentsMargins(PANE_MARGIN_X, PROMPT_GAP, PANE_MARGIN_X, PROMPT_GAP);

    auto* panel = new card;
    auto* line = new QWidget;
    panel->add_row(line);
    margins->addWidget(panel);

    auto* row = new QHBoxLayout(line);
    row->setContentsMargins(PROMPT_PADDING_X, PROMPT_PADDING_Y, PROMPT_PADDING_X, PROMPT_PADDING_Y);
    row->setSpacing(PROMPT_GAP);

    g_trial.ring = new countdown_ring;
    row->addWidget(g_trial.ring);

    auto* lines = new QVBoxLayout;
    lines->setSpacing(0);
    row->addLayout(lines, 1);

    g_trial.question = make_text(QString(), ui_font(STLXTHEME_BODY_SIZE, QFont::DemiBold), STLXTHEME_TEXT);
    g_trial.time_left = make_text(QString(), ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal), STLXTHEME_TEXT_DIM);
    lines->addWidget(g_trial.question);
    lines->addWidget(g_trial.time_left);

    g_trial.set_back = new push_button(QStringLiteral("Set back"));
    g_trial.set_back->on_click = [] { answer_trial(false); };
    row->addWidget(g_trial.set_back);

    g_trial.keep = new push_button(QStringLiteral("Keep"));
    g_trial.keep->set_primary(true);
    g_trial.keep->on_click = [] { answer_trial(true); };
    row->addWidget(g_trial.keep);

    /* Coarse ticks can come early and end the count before the display manager's timer */
    g_trial.countdown = new QTimer(prompt);
    g_trial.countdown->setTimerType(Qt::PreciseTimer);
    QObject::connect(g_trial.countdown, &QTimer::timeout, count_trial_down);

    g_trial.prompt = prompt;
    prompt->hide();

    return prompt;
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
