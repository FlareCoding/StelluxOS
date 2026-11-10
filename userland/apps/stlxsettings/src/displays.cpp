#include "kit.hpp"
#include "settings.hpp"

#include <stlxtheme/theme.h>
#include <stlxwin/stlxwin.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QSocketNotifier>
#include <QTimer>
#include <QVBoxLayout>

#include <cerrno>
#include <cstdlib>
#include <vector>

constexpr int ARRANGEMENT_H = 220;
constexpr int ARRANGEMENT_MARGIN = 16;
constexpr int OUTPUT_RADIUS = 6;
constexpr int MAIN_STRIP_PX = 4;
constexpr int MAIN_STRIP_INSET = 3;
constexpr int SNAP_PX = 24;

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

namespace {

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

static display_state g_display;
static trial_state g_trial;

static void set_dim(QWidget* widget) {
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, theme_color(STLXTHEME_TEXT_DIM));
    widget->setPalette(palette);
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
    if (page_stale) {
        rebuild_displays_page_later();
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

    /* A click can come before the page reads a MODES that made the chosen mode current, so Apply checks again */
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

void build_displays_page(QWidget* page) {
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

QWidget* make_keep_prompt() {
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
