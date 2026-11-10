#include "kit.hpp"
#include "settings.hpp"

#include <stlxtheme/theme.h>
#include <stlxwin/stlxwin.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QSocketNotifier>
#include <QTimer>
#include <QVBoxLayout>

#include <cerrno>
#include <cstdlib>
#include <vector>

constexpr int CANVAS_H = 184;
constexpr int CANVAS_MARGIN = 4;
constexpr int CANVAS_PADDING = 20;
constexpr int CANVAS_GRID = 16;
constexpr qreal CANVAS_DOT_RADIUS = 1.2;
constexpr qreal MAX_SCALE = 0.075;
constexpr int SNAP_PX = 24;
constexpr qreal SHARED_EDGE_DASH = 4;
constexpr qreal SHARED_EDGE_GAP = 3;

constexpr int TILE_RADIUS = 4;
constexpr int BADGE_SIZE = 18;
constexpr int BADGE_LEFT = 8;
constexpr int BADGE_TOP = 10;
constexpr int NAME_TOP = 38;
constexpr int NAME_INSET = 8;
constexpr int MODE_TOP = 58;
constexpr int BAR_STRIP_H = 5;
constexpr int DOCK_PILL_W = 30;
constexpr int DOCK_PILL_H = 6;
constexpr int DOCK_PILL_BOTTOM = 4;
constexpr int DOCK_PILL_RADIUS = 2;
constexpr int TAG_RIGHT = 8;
constexpr int TAG_TOP = 11;
constexpr qreal MILLIHERTZ_PER_HERTZ = 1000;

constexpr int NOTE_ROW_H = 44;
constexpr int NOTE_PADDING_LEFT = 14;
constexpr int NOTE_PADDING_RIGHT = 10;

constexpr int COUNTDOWN_TICK_MS = 1000;

/* The display manager sets a trial's mode back this long after lighting it, unless it is kept */
constexpr uint32_t TRIAL_SECONDS = 15;

/* A trial's keep prompt: a card above the page, the seconds left in a ring that empties with them */
constexpr int PROMPT_GAP = 12;
constexpr int PROMPT_PADDING_X = 12;
constexpr int PROMPT_PADDING_Y = 8;
constexpr int RING_PX = 26;
constexpr int RING_STROKE = 2;
constexpr int FULL_TURN = 360 * 16; /* `QPainter` arcs count sixteenths of a degree */
constexpr int RING_START = 90 * 16; /* the ring empties from its top */

constexpr uint32_t CANVAS_FILL = 0x4D080502;      /* the recess at .30 */
constexpr uint32_t CANVAS_DOT = 0x21E2C496;       /* brass at .13 */
constexpr uint32_t TILE_SMOKE = 0x80100D0A;       /* the wallpaper under half smoke */
constexpr uint32_t TILE_RING = 0x38FFF0D7;        /* cream at .22 */
constexpr uint32_t TILE_CONTACT = 0xBF060402;     /* ink at .75 */
constexpr uint32_t TILE_CHROME = 0xE012100D;      /* the bar and dock hinted on the main tile, the warm tint at .88 */
constexpr uint32_t TILE_CHROME_LINE = 0x40E2C496; /* brass at .25 */
constexpr uint32_t BADGE_FILL = 0x1FFFF0D7;       /* cream at .12 */

static QString display_name(const stlxwin_output& output, uint32_t index);
static QString mode_text(const stlxwin_output& output);
static bool has_layout_changes();
static void follow_outputs();

namespace {

/* The Displays page's arrangement, mode choices and own connection to the display manager, since libstlxqpa drops
 * layout and mode results */
struct display_state {
    stlxwin_conn* conn = nullptr;
    QSocketNotifier* notifier = nullptr;
    uint32_t generation = 0;
    uint32_t count = 0;
    stlxwin_output outputs[STLXWIN_OUTPUTS_MAX] = {};
    QPoint top_lefts[STLXWIN_OUTPUTS_MAX];
    uint32_t main_output = 0;
    uint32_t listed_main = 0;                            /* stlxdm's main output, which a pending change differs from */
    uint32_t selected = 0;                               /* the selected tile's index in `outputs` */
    uint32_t mode_watches = 0;                           /* outputs whose modes the connection asked for, by bit */
    uint32_t unchangeable_outputs = 0;                   /* outputs whose mode request was answered `-ENOTTY`, by bit */
    stlxwin_mode chosen_modes[STLXWIN_OUTPUTS_MAX] = {}; /* each output's pick, its current mode again at its MODES */
};

/* The outputs drawn to scale on a dotted canvas, each a tile of the wallpaper under smoke. A dragged one snaps
 * to edges of the others within `SNAP_PX` canvas pixels, and a dashed line marks an edge it shares */
class arrangement_view : public QWidget {
public:
    explicit arrangement_view(display_state* display) : m_display(display) {
        setFixedHeight(CANVAS_H + 2 * CANVAS_MARGIN);
    }

    /* Fired when a press selects a display and when a drag ends */
    std::function<void()> on_change;

protected:
    void paintEvent(QPaintEvent*) override {
        /* A drag holds the scale, so the pointer moves only the dragged output */
        if (m_dragged < 0) {
            scale_to_view();
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        paint_canvas(painter);

        for (uint32_t i = 0; i < m_display->count; i++) {
            if (static_cast<int>(i) != m_dragged) {
                paint_tile(painter, i);
            }
        }

        if (m_dragged >= 0) {
            paint_shared_edges(painter, static_cast<uint32_t>(m_dragged));
            paint_tile(painter, static_cast<uint32_t>(m_dragged));
        }

        if (m_display->count > 0) {
            paint_selection_ring(painter);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        m_dragged = output_at(event->position());
        if (m_dragged < 0) {
            return;
        }

        m_display->selected = static_cast<uint32_t>(m_dragged);
        m_grab_top_left = m_display->top_lefts[m_dragged];
        m_grab_pointer = event->position();
        update();

        if (on_change) {
            on_change();
        }
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
        bool was_dragging = m_dragged >= 0;
        m_dragged = -1;
        update();

        if (was_dragging && on_change) {
            on_change();
        }
    }

private:
    QRectF canvas_rect() const {
        return QRectF(rect()).adjusted(CANVAS_MARGIN, CANVAS_MARGIN, -CANVAS_MARGIN, -CANVAS_MARGIN);
    }

    QRect place_rect(uint32_t i) const {
        const stlxwin_output& output = m_display->outputs[i];
        return QRect(m_display->top_lefts[i], QSize(static_cast<int>(output.width), static_cast<int>(output.height)));
    }

    QRectF canvas_place_rect(uint32_t i) const {
        QRectF place(place_rect(i));
        return QRectF(m_origin + place.topLeft() * m_scale, place.size() * m_scale);
    }

    QRectF tile_rect(uint32_t i) const {
        return canvas_place_rect(i).adjusted(1, 1, -1, -1);
    }

    int output_at(QPointF position) const {
        for (uint32_t i = 0; i < m_display->count; i++) {
            if (canvas_place_rect(i).contains(position)) {
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

        QRectF room = canvas_rect().adjusted(CANVAS_PADDING, CANVAS_PADDING, -CANVAS_PADDING, -CANVAS_PADDING);
        if (bounds.isEmpty() || room.isEmpty()) {
            return;
        }

        m_scale = qMin(MAX_SCALE, qMin(room.width() / bounds.width(), room.height() / bounds.height()));
        m_origin = canvas_rect().center() - QRectF(bounds).center() * m_scale;
    }

    /* The shortest of `shift` and the four shifts that put an end of the span on an end of the other */
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

    static bool is_side_by_side(QRect a, QRect b) {
        return a.x() + a.width() == b.x() || b.x() + b.width() == a.x();
    }

    /* Whether `a` and `b` touch along an edge at least one pixel long */
    static bool share_edge(QRect a, QRect b) {
        bool rows_meet = qMin(a.y() + a.height(), b.y() + b.height()) > qMax(a.y(), b.y());
        bool columns_meet = qMin(a.x() + a.width(), b.x() + b.width()) > qMax(a.x(), b.x());
        bool stacked = a.y() + a.height() == b.y() || b.y() + b.height() == a.y();

        return (is_side_by_side(a, b) && rows_meet) || (stacked && columns_meet);
    }

    /* Whether `moving` overlaps no other output and, with `dragged` there, every output reaches the others
     * through shared edges */
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
        bool has_spread = true;
        while (has_spread) {
            has_spread = false;
            for (uint32_t i = 0; i < m_display->count; i++) {
                if (!reached[i]) {
                    continue;
                }

                for (uint32_t j = 0; j < m_display->count; j++) {
                    if (!reached[j] && share_edge(places[i], places[j])) {
                        reached[j] = true;
                        joined++;
                        has_spread = true;
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
        bool is_x_shorter = std::abs(dx) < std::abs(dy);
        QPoint both(dx, dy);
        QPoint shorter = is_x_shorter ? QPoint(dx, 0) : QPoint(0, dy);
        QPoint longer = is_x_shorter ? QPoint(0, dy) : QPoint(dx, 0);

        /* Both snaps can leave two outputs meeting only at a corner, which does not join them, so the first
         * of these that fits applies */
        for (QPoint shift : { both, shorter, longer }) {
            if (fits(moving.translated(shift), dragged)) {
                return top_left + shift;
            }
        }

        return top_left + both;
    }

    /* The dot grid is drawn once for each canvas size, since it holds hundreds of dots */
    void paint_canvas(QPainter& painter) {
        QRectF canvas = canvas_rect();
        if (m_grid.size() != canvas.size().toSize()) {
            m_grid = QPixmap(canvas.size().toSize());
            m_grid.fill(Qt::transparent);
            QPainter grid(&m_grid);

            grid.setRenderHint(QPainter::Antialiasing);
            grid.setPen(Qt::NoPen);
            grid.setBrush(theme_color(CANVAS_DOT));
            for (qreal y = CANVAS_GRID / 2.0; y < canvas.height(); y += CANVAS_GRID) {
                for (qreal x = CANVAS_GRID / 2.0; x < canvas.width(); x += CANVAS_GRID) {
                    grid.drawEllipse(QPointF(x, y), CANVAS_DOT_RADIUS, CANVAS_DOT_RADIUS);
                }
            }
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme_color(CANVAS_FILL));
        painter.drawRoundedRect(canvas, STLXTHEME_THUMBNAIL_RADIUS, STLXTHEME_THUMBNAIL_RADIUS);
        painter.drawPixmap(canvas.topLeft(), m_grid);

        painter.setPen(QPen(theme_color(STLXTHEME_LINE_SUBTLE), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(
            canvas.adjusted(0.5, 0.5, -0.5, -0.5),
            STLXTHEME_THUMBNAIL_RADIUS - 0.5,
            STLXTHEME_THUMBNAIL_RADIUS - 0.5
        );
    }

    void paint_picture(QPainter& painter, const QRectF& tile) const {
        QBrush picture(wallpaper_picture(tile.size().toSize()));
        picture.setTransform(QTransform::fromTranslate(tile.left(), tile.top()));

        painter.setPen(QPen(theme_color(TILE_CONTACT), 1));
        painter.setBrush(picture);
        painter.drawRoundedRect(tile.adjusted(-0.5, -0.5, 0.5, 0.5), TILE_RADIUS + 0.5, TILE_RADIUS + 0.5);

        painter.setPen(QPen(theme_color(TILE_RING), 1));
        painter.setBrush(theme_color(TILE_SMOKE));
        painter.drawRoundedRect(tile.adjusted(0.5, 0.5, -0.5, -0.5), TILE_RADIUS - 0.5, TILE_RADIUS - 0.5);
    }

    static bool has_name_room(qreal height) {
        return height >= NAME_TOP + BODY_LINE_HEIGHT;
    }

    static bool has_mode_room(qreal height) {
        return height >= MODE_TOP + CAPTION_LINE_HEIGHT;
    }

    /* The main tile hints the bar along its top and the dock at its bottom, as the wallpaper preview does */
    void paint_main_hints(QPainter& painter, const QRectF& tile) const {
        QPainterPath outline;
        outline.addRoundedRect(tile, TILE_RADIUS, TILE_RADIUS);
        QPainterPath strip;
        strip.addRect(QRectF(tile.left(), tile.top(), tile.width(), BAR_STRIP_H));

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme_color(TILE_CHROME));
        painter.drawPath(outline.intersected(strip));
        painter.fillRect(QRectF(tile.left(), tile.top() + BAR_STRIP_H, tile.width(), 1), theme_color(TILE_CHROME_LINE));

        int text_bottom = BADGE_TOP + BADGE_SIZE;
        if (has_mode_room(tile.height())) {
            text_bottom = MODE_TOP + CAPTION_LINE_HEIGHT;
        } else if (has_name_room(tile.height())) {
            text_bottom = NAME_TOP + BODY_LINE_HEIGHT;
        }

        bool has_dock_room = tile.height() >= text_bottom + DOCK_PILL_BOTTOM + DOCK_PILL_H;
        if (has_dock_room) {
            QRectF dock(
                tile.center().x() - DOCK_PILL_W / 2.0,
                tile.bottom() - DOCK_PILL_BOTTOM - DOCK_PILL_H,
                DOCK_PILL_W,
                DOCK_PILL_H
            );
            painter.setPen(QPen(theme_color(TILE_CHROME_LINE), 1));
            painter.setBrush(theme_color(TILE_CHROME));
            painter.drawRoundedRect(
                dock.adjusted(-0.5, -0.5, 0.5, 0.5),
                DOCK_PILL_RADIUS + 0.5,
                DOCK_PILL_RADIUS + 0.5
            );
        }

        QFont tag_font = caps_font();
        QString tag = QStringLiteral("MAIN");
        qreal tag_width = QFontMetricsF(tag_font).horizontalAdvance(tag);
        bool has_tag_room = tile.width() >= BADGE_LEFT + BADGE_SIZE + tag_width + TAG_RIGHT;
        if (!has_tag_room) {
            return;
        }

        QRectF tag_line(tile.left(), tile.top() + TAG_TOP, tile.width() - TAG_RIGHT, CAPTION_LINE_HEIGHT);
        painter.setFont(tag_font);
        painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
        painter.drawText(tag_line, Qt::AlignRight | Qt::AlignVCenter, tag);
    }

    void paint_tile(QPainter& painter, uint32_t i) const {
        const stlxwin_output& output = m_display->outputs[i];
        QRectF tile = tile_rect(i);
        paint_picture(painter, tile);

        if (output.output == m_display->main_output) {
            paint_main_hints(painter, tile);
        }

        QRectF badge(tile.left() + BADGE_LEFT, tile.top() + BADGE_TOP, BADGE_SIZE, BADGE_SIZE);
        painter.setPen(QPen(theme_color(STLXTHEME_LINE), 1));
        painter.setBrush(theme_color(BADGE_FILL));
        painter.drawRoundedRect(
            badge.adjusted(0.5, 0.5, -0.5, -0.5),
            STLXTHEME_CHIP_RADIUS - 0.5,
            STLXTHEME_CHIP_RADIUS - 0.5
        );

        painter.setFont(mono_font(STLXTHEME_CAPTION_SIZE));
        painter.setPen(theme_color(STLXTHEME_TEXT));
        painter.drawText(badge, Qt::AlignCenter, QString::number(i + 1));

        if (!has_name_room(tile.height())) {
            return;
        }

        QRectF name_line(tile.left(), tile.top() + NAME_TOP, tile.width(), BODY_LINE_HEIGHT);
        QFont name_font = ui_font(STLXTHEME_BODY_SIZE, QFont::DemiBold);
        painter.setFont(name_font);
        qreal name_width = tile.width() - 2 * NAME_INSET;
        QString name = QFontMetricsF(name_font).elidedText(display_name(output, i), Qt::ElideRight, name_width);
        painter.drawText(name_line, Qt::AlignCenter, name);

        if (!has_mode_room(tile.height())) {
            return;
        }

        QRectF mode_line(tile.left(), tile.top() + MODE_TOP, tile.width(), CAPTION_LINE_HEIGHT);
        QFont mode_font = mono_font(STLXTHEME_CAPTION_SIZE);
        painter.setFont(mode_font);
        painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
        QString mode = QFontMetricsF(mode_font).elidedText(mode_text(output), Qt::ElideRight, tile.width());
        painter.drawText(mode_line, Qt::AlignCenter, mode);
    }

    /* Drawn after every tile, since the ring reaches past the gap between neighbors */
    void paint_selection_ring(QPainter& painter) const {
        QRectF tile = tile_rect(m_display->selected);
        qreal reach = SELECTION_RING_OFFSET + SELECTION_RING_WIDTH / 2;

        painter.setPen(QPen(theme_color(STLXTHEME_TEXT), SELECTION_RING_WIDTH));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(tile.adjusted(-reach, -reach, reach, reach), TILE_RADIUS + reach, TILE_RADIUS + reach);
    }

    void paint_shared_edges(QPainter& painter, uint32_t dragged) const {
        QRect moving = place_rect(dragged);
        QPen dashes(theme_color(STLXTHEME_TEXT), 1);
        dashes.setDashPattern({ SHARED_EDGE_DASH, SHARED_EDGE_GAP });
        painter.setPen(dashes);

        for (uint32_t i = 0; i < m_display->count; i++) {
            QRect other = place_rect(i);
            if (i == dragged || !share_edge(moving, other)) {
                continue;
            }

            bool is_vertical_edge = is_side_by_side(moving, other);
            int edge = is_vertical_edge ? qMax(moving.left(), other.left()) : qMax(moving.top(), other.top());
            int start = is_vertical_edge ? qMax(moving.top(), other.top()) : qMax(moving.left(), other.left());
            int end = is_vertical_edge ? qMin(moving.bottom(), other.bottom()) + 1
                                       : qMin(moving.right(), other.right()) + 1;

            QPointF from = is_vertical_edge ? QPointF(edge, start) : QPointF(start, edge);
            QPointF to = is_vertical_edge ? QPointF(edge, end) : QPointF(end, edge);
            painter.drawLine(m_origin + from * m_scale, m_origin + to * m_scale);
        }
    }

    display_state* m_display;
    QPixmap m_grid;
    qreal m_scale = 1;
    QPointF m_origin;
    int m_dragged = -1;
    QPoint m_grab_top_left;
    QPointF m_grab_pointer;
};

/* An output's resolution and refresh rate choices and the Apply that tries them, filled in place */
struct mode_rows {
    setting_row* resolution_row = nullptr;
    setting_row* refresh_row = nullptr;
    QComboBox* resolution = nullptr;
    QComboBox* refresh = nullptr;
    push_button* apply = nullptr;
    uint32_t output = 0;
    stlxwin_mode current = {};
    std::vector<stlxwin_mode> listed;
};

/* The selected display's card, updated in place as the selection or its place changes */
struct display_card {
    section_head* arrangement_head = nullptr; /* the Arrangement head, which shows the desktop's size */
    section_head* head = nullptr;
    mode_rows modes;
    setting_row* main_row = nullptr;
    toggle* main_switch = nullptr;
};

/* A trial's seconds left inside a ring that empties with them */
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

/* A display mode's request and its trial's keep prompt. The prompt sits above the page, which a trial's relight
 * rebuilds, so the rebuild does not delete it and it stays answerable from every page */
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
    stlxwin_mode previous = {}; /* the output's mode when the request went out, which a set-back returns to */
    uint32_t seconds_left = 0;
};

} // namespace

static display_state g_display;
static display_card g_card;
static trial_state g_trial;
static mode_rows g_unplaced_modes[STLXWIN_OUTPUTS_MAX]; /* the choices of lit outputs the desktop does not place */

/* The monitor's maker and name, or "Display" and its tile's number when it reports no identity */
static QString display_name(const stlxwin_output& output, uint32_t index) {
    size_t length = strnlen(output.monitor, sizeof(output.monitor));
    if (length == 0) {
        return QStringLiteral("Display %1").arg(index + 1);
    }

    return QString::fromUtf8(output.monitor, static_cast<qsizetype>(length));
}

static QString mode_text(const stlxwin_output& output) {
    QString size = QStringLiteral("%1 %2 %3").arg(output.width).arg(TIMES_SIGN).arg(output.height);
    if (output.refresh_millihertz == 0) {
        return size;
    }

    int hertz = qRound(output.refresh_millihertz / MILLIHERTZ_PER_HERTZ);

    return QStringLiteral("%1 %2 %3 Hz").arg(size).arg(MIDDLE_DOT).arg(hertz);
}

static QString layout_outcome_text(uint32_t outcome) {
    switch (outcome) {
    case STLXWIN_LAYOUT_APPLIED:
        return QStringLiteral("Layout applied and stored");
    case STLXWIN_LAYOUT_UNSAVED:
        return QStringLiteral("Layout applied but not stored, so it lasts until the display manager exits");
    case STLXWIN_LAYOUT_STALE:
        return QStringLiteral("Refused, the displays changed meanwhile, arrange them again");
    case STLXWIN_LAYOUT_INVALID:
        return QStringLiteral("Refused as invalid");
    case STLXWIN_LAYOUT_OVERLAP:
        return QStringLiteral("Refused, two displays overlap");
    case STLXWIN_LAYOUT_APART:
        return QStringLiteral("Refused, a display does not connect to the main display through shared edges");
    default:
        return QStringLiteral("Refused");
    }
}

static bool sizes_equal(const stlxwin_mode& a, const stlxwin_mode& b) {
    return a.width == b.width && a.height == b.height;
}

static bool modes_equal(const stlxwin_mode& a, const stlxwin_mode& b) {
    return sizes_equal(a, b) && a.refresh_millihertz == b.refresh_millihertz;
}

static QString size_text(const stlxwin_mode& mode) {
    return QStringLiteral("%1 %2 %3").arg(mode.width).arg(TIMES_SIGN).arg(mode.height);
}

/* A rate to the millihertz, so two rates that round to the same hertz stay apart */
static QString rate_text(uint64_t refresh_millihertz) {
    if (refresh_millihertz == 0) {
        return QStringLiteral("unknown rate");
    }

    return QStringLiteral("%1 Hz").arg(refresh_millihertz / MILLIHERTZ_PER_HERTZ, 0, 'f', 3);
}

/* What the display manager did, for an answer whose `result` is 0 */
static const char* mode_outcome_text(uint32_t outcome) {
    switch (outcome) {
    case STLXWIN_MODE_SET:
        return "Display mode set";
    case STLXWIN_MODE_TRIAL:
        return "Display mode on trial";
    case STLXWIN_MODE_KEPT:
        return "Display mode kept";
    case STLXWIN_MODE_SET_BACK:
        return "Display mode set back";
    case STLXWIN_MODE_DEFAULT:
        return "Setting the display mode back failed, so the output's default was set";
    case STLXWIN_MODE_NO_BUFFERS:
        return "Display mode set back, the desktop has no buffers at that size";
    case STLXWIN_MODE_BUSY:
        return "Refused, a trial is running";
    case STLXWIN_MODE_NO_TRIAL:
        return "Refused, the trial had already ended";
    case STLXWIN_MODE_INVALID:
        return "Refused as invalid";
    default:
        return "Answered";
    }
}

/* What did not happen when the display refused the mode the display manager asked for last */
static const char* mode_failure_text(uint32_t outcome) {
    switch (outcome) {
    case STLXWIN_MODE_SET:
        return "Display mode not set";
    case STLXWIN_MODE_TRIAL:
        return "Display mode not set, so no trial started";
    case STLXWIN_MODE_SET_BACK:
        return "Display mode not set back";
    case STLXWIN_MODE_DEFAULT:
        return "Display mode not set back, nor set to the output's default";
    case STLXWIN_MODE_NO_BUFFERS:
        return "The desktop has no buffers at that size, and setting the display mode back failed";
    default:
        return "The display refused a display mode";
    }
}

static QString mode_result_text(uint32_t outcome, int32_t result) {
    if (result == 0) {
        return QString::fromUtf8(mode_outcome_text(outcome));
    }

    if (result == -ENOTTY) {
        return QStringLiteral("Display mode not set, this display cannot change modes");
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
        set_status(QStringLiteral("The display mode changed and the trial ended"));
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

    bool had_layout_changes = has_layout_changes();
    g_display.count = 0;

    if (had_layout_changes) {
        show_pending_changes();
    }
}

/* While the Displays page shows, each `STLXWIN_EVT_OUTPUTS`, mode event or lost connection rebuilds it. The
 * footer shows layout and mode results */
static void read_display_events() {
    bool is_alive = stlxwin_dispatch(g_display.conn) >= 0;
    bool are_outputs_changed = false;
    bool is_mode_event_read = false;

    stlxwin_event event;
    while (is_alive && stlxwin_next_event(g_display.conn, &event)) {
        if (event.type == STLXWIN_EVT_DISCONNECTED) {
            is_alive = false;
        } else if (event.type == STLXWIN_EVT_OUTPUTS) {
            follow_outputs();
            are_outputs_changed = true;
        } else if (event.type == STLXWIN_EVT_LAYOUT_RESULT) {
            set_status(layout_outcome_text(event.layout_result.outcome));
        } else if (event.type == STLXWIN_EVT_MODES) {
            follow_modes(event.modes.output);
            is_mode_event_read = true;
        } else if (event.type == STLXWIN_EVT_MODE_RESULT) {
            follow_mode_result(event);
            is_mode_event_read = true;
        }
    }

    if (!is_alive) {
        close_display_connection();
    }

    bool is_page_stale = are_outputs_changed || is_mode_event_read || !is_alive;
    if (is_page_stale) {
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

/* The places and main output the display manager lists, the first output by default */
static void read_arrangement(uint32_t generation) {
    g_display.generation = generation;
    g_display.listed_main = g_display.outputs[0].output;
    g_display.selected = qMin(g_display.selected, g_display.count - 1);

    for (uint32_t i = 0; i < g_display.count; i++) {
        const stlxwin_output& output = g_display.outputs[i];
        g_display.top_lefts[i] = QPoint(output.x, output.y);
        if (output.flags & STLXWIN_DESKTOP_MAIN) {
            g_display.listed_main = output.output;
        }
    }

    g_display.main_output = g_display.listed_main;
}

/* stlxdm puts the main display at (0, 0), so moving the whole arrangement changes nothing */
static bool is_arrangement_moved() {
    QPoint shift = g_display.top_lefts[0] - QPoint(g_display.outputs[0].x, g_display.outputs[0].y);
    for (uint32_t i = 1; i < g_display.count; i++) {
        QPoint listed(g_display.outputs[i].x, g_display.outputs[i].y);
        if (g_display.top_lefts[i] - listed != shift) {
            return true;
        }
    }

    return false;
}

static bool is_main_changed() {
    return g_display.count > 0 && g_display.main_output != g_display.listed_main;
}

static bool has_layout_changes() {
    return is_arrangement_moved() || is_main_changed();
}

/* Lists the displays stlxdm places, returning their count, or -1 when it cannot be reached. Unsaved layout changes
 * last until Revert, a new list or a lost connection, so the rebuilds of Save and a page switch keep them */
static long list_displays() {
    bool is_fresh_connection = !g_display.conn;
    uint32_t generation = 0;
    long listed = -1;
    if (open_display_connection()) {
        listed = stlxwin_outputs_get(g_display.conn, &generation, g_display.outputs, STLXWIN_OUTPUTS_MAX);
    }

    g_display.count = static_cast<uint32_t>(qBound(0L, listed, static_cast<long>(STLXWIN_OUTPUTS_MAX)));
    bool is_new_list = is_fresh_connection || generation != g_display.generation;
    if (g_display.count > 0 && is_new_list) {
        read_arrangement(generation);
    }

    return listed;
}

/* The footer recounts only when a new list drops unsaved layout changes, so a mode text sent before the OUTPUTS
 * stays. An answer to a request comes after the OUTPUTS it causes, so its text replaces the count */
static void follow_outputs() {
    bool had_layout_changes = has_layout_changes();
    list_displays();

    if (had_layout_changes && !has_layout_changes()) {
        show_pending_changes();
    }
}

void note_layout_changes(QStringList& changes) {
    if (is_arrangement_moved()) {
        changes.append(QStringLiteral("Display arrangement changed"));
    }

    if (is_main_changed()) {
        changes.append(QStringLiteral("Main display changed"));
    }
}

/* A sent layout's outcome arrives as `STLXWIN_EVT_LAYOUT_RESULT`, which `read_display_events` shows in the footer */
void save_layout() {
    if (!g_display.conn || !has_layout_changes()) {
        return;
    }

    stlxwin_layout_entry entries[STLXWIN_OUTPUTS_MAX];
    for (uint32_t i = 0; i < g_display.count; i++) {
        entries[i] = { g_display.outputs[i].output, g_display.top_lefts[i].x(), g_display.top_lefts[i].y() };
    }

    int result = stlxwin_layout_set(
        g_display.conn,
        g_display.generation,
        g_display.main_output,
        entries,
        g_display.count
    );
    if (result != 0) {
        set_status(QStringLiteral("The layout could not be sent to the display manager"));
    }
}

void revert_layout() {
    read_arrangement(g_display.generation);
}

/* The desktop's size, the box around every display */
static QString arrangement_section() {
    QRect bounds;
    for (uint32_t i = 0; i < g_display.count; i++) {
        const stlxwin_output& output = g_display.outputs[i];
        bounds |= QRect(g_display.top_lefts[i], QSize(static_cast<int>(output.width), static_cast<int>(output.height)));
    }

    return QStringLiteral("[layout]  %1 %2 %3").arg(bounds.width()).arg(TIMES_SIGN).arg(bounds.height());
}

/* The output's `display_name` when the desktop places it, or "Output" and its output number */
static QString output_title(uint32_t output) {
    for (uint32_t i = 0; i < g_display.count; i++) {
        if (g_display.outputs[i].output == output) {
            return display_name(g_display.outputs[i], i);
        }
    }

    return QStringLiteral("Output %1").arg(output);
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
        set_status(QStringLiteral("The display mode could not be sent to the display manager"));
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
    QSignalBlocker blocker(choice);
    choice->clear();

    for (size_t i = 0; i < listed.size(); i++) {
        if (!is_first_of_size(listed, i)) {
            continue;
        }

        QString text = size_text(listed[i]);
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

/* Reads the modes of `output` into `modes`, returning their count, or 0 while it is dark, past the output count or
 * its modes have not arrived */
static long read_lit_modes(uint32_t output, stlxwin_modes_info& info, stlxwin_mode* modes) {
    long count = stlxwin_modes_get(g_display.conn, output, &info, modes, STLXWIN_MODES_MAX);
    bool is_lit = count > 0 && (info.flags & STLXWIN_OUTPUT_LIT) != 0;

    return is_lit ? count : 0;
}

/* Fills `rows` with the modes of `output`, hiding them while `read_lit_modes` finds none */
static void show_modes(mode_rows& rows, uint32_t output) {
    stlxwin_modes_info info;
    stlxwin_mode modes[STLXWIN_MODES_MAX];
    long count = read_lit_modes(output, info, modes);
    rows.resolution_row->setVisible(count > 0);
    rows.refresh_row->setVisible(count > 0);
    if (count == 0) {
        return;
    }

    /* A choice made from a list that a later MODES replaced may not be listed any more */
    rows.output = output;
    rows.current = info.current;
    rows.listed.assign(modes, modes + count);
    if (!is_listed(rows.listed, g_display.chosen_modes[output])) {
        g_display.chosen_modes[output] = info.current;
    }

    fill_resolution_choice(rows.resolution, rows.listed, g_display.chosen_modes[output]);
    fill_refresh_choice(rows.refresh, rows.listed, g_display.chosen_modes[output]);

    bool is_greyed = count == 1 || (g_display.unchangeable_outputs & (1u << output)) != 0;
    rows.resolution->setEnabled(!is_greyed);
    rows.refresh->setEnabled(!is_greyed);
    rows.apply->setEnabled(!is_greyed && can_apply_mode(output, info.current));
}

/* Adds the resolution and refresh rate rows to `details`, kept in `rows` for `show_modes` to fill. The rows'
 * handlers read `rows`, so it must outlive them */
static void add_mode_rows(card* details, mode_rows& rows) {
    rows.resolution = new QComboBox;
    rows.resolution_row = new setting_row(QStringLiteral("Resolution"), QString());
    rows.resolution_row->add_control(rows.resolution);
    details->add_row(rows.resolution_row);

    rows.refresh = new QComboBox;
    rows.apply = new push_button(QStringLiteral("Apply"));
    rows.refresh_row = new setting_row(QStringLiteral("Refresh rate"), QString());
    rows.refresh_row->add_control(rows.refresh);
    rows.refresh_row->add_control(rows.apply);
    details->add_row(rows.refresh_row);

    mode_rows* shown = &rows;
    rows.apply->on_click = [shown] { apply_mode(shown->output); };

    QObject::connect(rows.resolution, &QComboBox::currentIndexChanged, [shown](int index) {
        g_display.chosen_modes[shown->output] = shown->listed[shown->resolution->itemData(index).toUInt()];
        fill_refresh_choice(shown->refresh, shown->listed, g_display.chosen_modes[shown->output]);
        shown->apply->setEnabled(can_apply_mode(shown->output, shown->current));
    });

    QObject::connect(rows.refresh, &QComboBox::currentIndexChanged, [shown](int index) {
        g_display.chosen_modes[shown->output] = shown->listed[shown->refresh->itemData(index).toUInt()];
        shown->apply->setEnabled(can_apply_mode(shown->output, shown->current));
    });
}

/* Where stlxdm stores display `index` once the layout is saved, since it puts the main display at (0, 0) */
static QPoint place_relative_to_main(uint32_t index) {
    QPoint main_place;
    for (uint32_t i = 0; i < g_display.count; i++) {
        if (g_display.outputs[i].output == g_display.main_output) {
            main_place = g_display.top_lefts[i];
        }
    }

    return g_display.top_lefts[index] - main_place;
}

static void show_selected_display() {
    uint32_t selected = g_display.selected;
    const stlxwin_output& output = g_display.outputs[selected];
    QPoint place = place_relative_to_main(selected);
    QString title = QStringLiteral("Display %1").arg(selected + 1);
    if (output.monitor[0] != '\0') {
        title += QStringLiteral(" %1 %2").arg(MIDDLE_DOT).arg(display_name(output, selected));
    }

    g_card.arrangement_head->set_caption(arrangement_section());
    g_card.head->set_title(title);
    g_card.head->set_caption(QStringLiteral("x = %1   y = %2").arg(place.x()).arg(place.y()));
    g_card.main_row->set_key(QStringLiteral("main = %1").arg(g_display.main_output));
    g_card.main_switch->set_on(output.output == g_display.main_output);
    show_modes(g_card.modes, output.output);
}

static QWidget* make_note_row(const QString& text) {
    auto* row = new QWidget;
    row->setFixedHeight(NOTE_ROW_H);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(NOTE_PADDING_LEFT, 0, NOTE_PADDING_RIGHT, 0);
    layout->addWidget(make_text(text, ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal), STLXTHEME_TEXT_DIM));
    return row;
}

/* Only another display can take over as main, so switching the main one off leaves it on */
static void add_main_display_row(card* details, arrangement_view* view) {
    g_card.main_row = new setting_row(QStringLiteral("Main display"), QString());
    g_card.main_switch = new toggle(false);
    QLabel* hint = make_text(
        QStringLiteral("Bar and dock live here"),
        ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal),
        STLXTHEME_TEXT_DIM
    );
    g_card.main_row->add_control(hint);
    g_card.main_row->add_control(g_card.main_switch);
    details->add_row(g_card.main_row);

    g_card.main_switch->on_toggle = [view](bool is_on) {
        if (is_on) {
            g_display.main_output = g_display.outputs[g_display.selected].output;
        }

        show_selected_display();
        view->update();
        show_pending_changes();
    };
}

static bool is_placed(uint32_t output) {
    for (uint32_t i = 0; i < g_display.count; i++) {
        if (g_display.outputs[i].output == output) {
            return true;
        }
    }

    return false;
}

/* A lit output the desktop does not place has no tile, so its choices take a section of their own */
static void add_unplaced_outputs(QWidget* page) {
    for (uint32_t output = 0; output < STLXWIN_OUTPUTS_MAX; output++) {
        stlxwin_modes_info info;
        stlxwin_mode modes[STLXWIN_MODES_MAX];
        if (is_placed(output) || read_lit_modes(output, info, modes) == 0) {
            continue;
        }

        card* details = add_section(page, output_title(output), QString());
        add_mode_rows(details, g_unplaced_modes[output]);
        show_modes(g_unplaced_modes[output], output);
    }
}

void build_displays_page(QWidget* page) {
    long listed = list_displays();

    g_card.arrangement_head = new section_head(QStringLiteral("Arrangement"), QStringLiteral("[layout]"));
    card* arrangement = add_section(page, g_card.arrangement_head);
    if (g_display.count == 0) {
        arrangement->add_row(make_note_row(listed < 0 ? QStringLiteral("The display manager could not be reached")
                                                      : QStringLiteral("The display manager lists no displays")));
        return;
    }

    auto* view = new arrangement_view(&g_display);
    view->on_change = []() {
        show_selected_display();
        show_pending_changes();
    };

    arrangement->add_row(view);
    arrangement->add_row(make_note_row(QStringLiteral("Drag a display to place it. Edges snap to their neighbours.")));

    g_card.head = new section_head(QString(), QString());
    card* details = add_section(page, g_card.head);
    add_mode_rows(details, g_card.modes);
    add_main_display_row(details, view);
    show_selected_display();
    add_unplaced_outputs(page);
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
        set_status(QStringLiteral("The answer could not be sent to the display manager"));
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
