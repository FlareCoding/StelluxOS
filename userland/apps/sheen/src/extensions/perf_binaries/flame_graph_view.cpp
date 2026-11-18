#include "extensions/perf_binaries/flame_graph_view.h"
#include "extensions/perf_binaries/elf_hex_view.h"
#include "ui/chrome.h"
#include "ui/ui_kit.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace sheen {

static constexpr uint32_t FLAME_ROOT_STONE = 0xFF26201C;
static constexpr uint32_t FLAME_FRAME_SAND = 0xFF3B312B;
static constexpr uint32_t FLAME_HOT_COPPER = 0xFFA65A38;
static constexpr uint32_t FLAME_TEXT_SAND = 0xFFC9B69E;
static constexpr uint32_t SIGNAL_AMBER = 0xFFE09B2D;
static constexpr uint32_t STATE_RUNNING_FILL = 0xFF8A6544;
static constexpr uint32_t STATE_RUNNABLE_FILL = 0xFF56483A;
static constexpr uint32_t STATE_SLEEPING_FILL = 0xFF1D1815;

static constexpr int OUTSIDE_SHADE_ALPHA = 130;
static constexpr int HEADER_BAR_HEIGHT = 34;
static constexpr int FOOTER_BAR_HEIGHT = 28;
static constexpr int HEADER_PADDING_X = 12;
static constexpr int HEADER_GAP = 8;
static constexpr int TRACE_MAGIC_READ_BYTES = 16;

static constexpr int CANVAS_PAD_X = 14;
static constexpr int TIMELINE_TOP_Y = 10;
static constexpr int TIMELINE_BARS_H = 44;
static constexpr int STATE_LANE_GAP_Y = 6;
static constexpr int STATE_LANE_H = 14;
static constexpr int FLAME_TOP_OFFSET_Y = 92;
static constexpr int FLAME_BAR_H = 22;
static constexpr int FLAME_ROW_STEP_Y = 24;
static constexpr qreal MIN_SPAN_DRAW_PX = 1.0;
static constexpr qreal MIN_SPAN_TEXT_PX = 40.0;
static constexpr qreal BAR_CORNER_RADIUS = 3.0;
static constexpr qreal SELECTION_STROKE_PX = 1.5;

static constexpr size_t TIMELINE_BUCKET_COUNT = 96;
static constexpr uint64_t MIN_ZOOM_WINDOW_NS = 1000ull;
static constexpr uint64_t FRAME_BUDGET_240HZ_NS = 4166667ull;
static constexpr double MAX_COPPER_BLEND = 0.72;
static constexpr double WHEEL_ZOOM_IN_FACTOR = 0.80;
static constexpr double WHEEL_ZOOM_OUT_FACTOR = 1.25;

static uint64_t g_temp_db_counter = 0;

class flame_timeline_canvas final : public QWidget {
public:
    explicit flame_timeline_canvas(QWidget* parent = nullptr);

    void set_status_message(const QString& message);
    void set_scroll_row_offset(int row_offset);
    int visible_flame_rows() const;
    const ktrace_flame_span* selected_span() const;

    void set_render_state(const ktrace_profile_model* model,
                          const ktrace_window_view* window,
                          const std::vector<ktrace_timeline_bar>* bars,
                          const std::vector<ktrace_thread_frame>* frames,
                          const std::vector<ktrace_thread_state>* lane_states,
                          uint64_t zoom_start_ns,
                          uint64_t zoom_end_ns);

    std::function<void(uint64_t start_ns, uint64_t end_ns)> on_zoom_dragged;
    std::function<void()> on_zoom_reset;
    std::function<void(double anchor_fraction, double scale)> on_wheel_zoom;
    std::function<void(int row_delta)> on_vertical_scroll;
    std::function<void()> on_viewport_resized;
    std::function<void(const ktrace_flame_span& span, bool is_double_click)> on_span_clicked;

protected:
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    uint64_t trace_time_at_x(int px) const;
    int span_index_at(const QPoint& pos) const;
    void paint_timeline_band(QPainter& painter, qreal inner_w);
    void paint_flame_spans(QPainter& painter, qreal inner_w);
    void paint_hover_card(QPainter& painter);

    const ktrace_profile_model* m_model = nullptr;
    const ktrace_window_view* m_window = nullptr;
    const std::vector<ktrace_timeline_bar>* m_bars = nullptr;
    const std::vector<ktrace_thread_frame>* m_frames = nullptr;
    const std::vector<ktrace_thread_state>* m_lane_states = nullptr;
    uint64_t m_zoom_start_ns = 0;
    uint64_t m_zoom_end_ns = 0;
    uint64_t m_drag_start_ns = 0;
    bool m_is_dragging_timeline = false;
    int m_scroll_row_offset = 0;
    int m_hovered_span = -1;
    int m_selected_span = -1;
    QPoint m_hover_pos;
    QString m_message;
};

static QString qstring_duration_ns(uint64_t ns) {
    return QString::fromStdString(format_duration_ns(ns));
}

static QColor blend_rgb(uint32_t base_argb, uint32_t tint_argb, double ratio) {
    double clamped = std::clamp(ratio, 0.0, 1.0);
    QColor base = token(base_argb);
    QColor tint = token(tint_argb);

    int red = static_cast<int>(std::lround(base.red() * (1.0 - clamped) + tint.red() * clamped));
    int green = static_cast<int>(std::lround(base.green() * (1.0 - clamped) + tint.green() * clamped));
    int blue = static_cast<int>(std::lround(base.blue() * (1.0 - clamped) + tint.blue() * clamped));
    return QColor(red, green, blue);
}

static QString format_health_label(const ktrace_profile_model& model) {
    if (model.all_health_checks_passed) {
        return QStringLiteral("Health OK");
    }

    if (model.session.lost_records > 0) {
        return QStringLiteral("Lost %1").arg(model.session.lost_records);
    }

    for (const ktrace_health_status& check : model.health_checks) {
        if (!check.passed && !check.name.empty()) {
            return QString::fromStdString(check.name) + QStringLiteral(" failed");
        }
    }

    return QStringLiteral("Incomplete");
}

flame_timeline_canvas::flame_timeline_canvas(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
}

void flame_timeline_canvas::set_status_message(const QString& message) {
    m_message = message;
    update();
}

void flame_timeline_canvas::set_scroll_row_offset(int row_offset) {
    int clamped = std::max(0, row_offset);
    if (m_scroll_row_offset != clamped) {
        m_scroll_row_offset = clamped;
        m_hovered_span = -1;
        update();
    }
}

int flame_timeline_canvas::visible_flame_rows() const {
    return std::max(1, (height() - FLAME_TOP_OFFSET_Y) / FLAME_ROW_STEP_Y);
}

const ktrace_flame_span* flame_timeline_canvas::selected_span() const {
    if (!m_window || m_selected_span < 0 ||
        static_cast<size_t>(m_selected_span) >= m_window->flame_spans.size()) {
        return nullptr;
    }

    return &m_window->flame_spans[static_cast<size_t>(m_selected_span)];
}

void flame_timeline_canvas::set_render_state(const ktrace_profile_model* model,
                                             const ktrace_window_view* window,
                                             const std::vector<ktrace_timeline_bar>* bars,
                                             const std::vector<ktrace_thread_frame>* frames,
                                             const std::vector<ktrace_thread_state>* lane_states,
                                             uint64_t zoom_start_ns,
                                             uint64_t zoom_end_ns) {
    m_model = model;
    m_window = window;
    m_bars = bars;
    m_frames = frames;
    m_lane_states = lane_states;

    m_zoom_start_ns = zoom_start_ns;
    m_zoom_end_ns = zoom_end_ns;
    m_hovered_span = -1;
    m_selected_span = -1;
    m_message.clear();
    update();
}

void flame_timeline_canvas::leaveEvent(QEvent*) {
    if (m_hovered_span != -1) {
        m_hovered_span = -1;
        update();
    }
}

void flame_timeline_canvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (on_viewport_resized) {
        on_viewport_resized();
    }
}

void flame_timeline_canvas::wheelEvent(QWheelEvent* event) {
    if (!m_model || event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    if ((event->modifiers() & Qt::ShiftModifier) && on_vertical_scroll) {
        on_vertical_scroll(event->angleDelta().y() > 0 ? -2 : 2);
        event->accept();
        return;
    }

    if (on_wheel_zoom) {
        qreal inner_w = std::max<qreal>(1.0, width() - 2 * CANVAS_PAD_X);
        double anchor_fraction = std::clamp((event->position().x() - CANVAS_PAD_X) / inner_w, 0.0, 1.0);
        double factor = event->angleDelta().y() > 0 ? WHEEL_ZOOM_IN_FACTOR : WHEEL_ZOOM_OUT_FACTOR;
        on_wheel_zoom(anchor_fraction, factor);
        event->accept();
    }
}

void flame_timeline_canvas::mousePressEvent(QMouseEvent* event) {
    if (!m_model || event->button() != Qt::LeftButton) {
        return;
    }

    if (event->pos().y() < FLAME_TOP_OFFSET_Y - 6) {
        m_is_dragging_timeline = true;
        m_drag_start_ns = trace_time_at_x(event->pos().x());
        return;
    }

    int hit = span_index_at(event->pos());
    m_selected_span = hit;
    update();
    if (hit >= 0 && m_window && on_span_clicked) {
        on_span_clicked(m_window->flame_spans[static_cast<size_t>(hit)], false);
    }
}

void flame_timeline_canvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!m_model || event->button() != Qt::LeftButton) {
        return;
    }

    if (event->pos().y() < FLAME_TOP_OFFSET_Y - 6) {
        if (on_zoom_reset) {
            on_zoom_reset();
        }

        return;
    }

    int hit = span_index_at(event->pos());
    if (hit >= 0 && m_window && on_span_clicked) {
        m_selected_span = hit;
        update();
        on_span_clicked(m_window->flame_spans[static_cast<size_t>(hit)], true);
    }
}

void flame_timeline_canvas::mouseMoveEvent(QMouseEvent* event) {
    if (!m_model) {
        return;
    }

    QPoint prev_pos = m_hover_pos;
    m_hover_pos = event->pos();
    if (m_is_dragging_timeline && on_zoom_dragged) {
        uint64_t cur_ns = trace_time_at_x(event->pos().x());
        uint64_t lo = std::min(m_drag_start_ns, cur_ns);
        uint64_t hi = std::max(m_drag_start_ns, cur_ns);
        if (hi > lo + MIN_ZOOM_WINDOW_NS) {
            on_zoom_dragged(lo, hi);
        }

        return;
    }

    int hit = event->pos().y() >= FLAME_TOP_OFFSET_Y ? span_index_at(event->pos()) : -1;
    if (hit != m_hovered_span || (hit >= 0 && prev_pos != m_hover_pos)) {
        m_hovered_span = hit;
        update();
    }
}

void flame_timeline_canvas::mouseReleaseEvent(QMouseEvent*) {
    m_is_dragging_timeline = false;
}

uint64_t flame_timeline_canvas::trace_time_at_x(int px) const {
    if (!m_model) {
        return 0;
    }

    uint64_t t0 = m_model->session.start_ns;
    uint64_t t1 = std::max(t0 + 1, m_model->session.effective_end_ns);
    double inner_w = std::max(1.0, static_cast<double>(width() - 2 * CANVAS_PAD_X));
    double fraction = std::clamp(static_cast<double>(px - CANVAS_PAD_X) / inner_w, 0.0, 1.0);
    return t0 + static_cast<uint64_t>(std::llround(fraction * static_cast<double>(t1 - t0)));
}

int flame_timeline_canvas::span_index_at(const QPoint& pos) const {
    if (!m_window || pos.y() < FLAME_TOP_OFFSET_Y) {
        return -1;
    }

    int visible_row = (pos.y() - FLAME_TOP_OFFSET_Y) / FLAME_ROW_STEP_Y;
    if (((pos.y() - FLAME_TOP_OFFSET_Y) % FLAME_ROW_STEP_Y) > FLAME_BAR_H) {
        return -1;
    }

    uint32_t depth = static_cast<uint32_t>(visible_row + m_scroll_row_offset);
    double inner_w = std::max(1.0, static_cast<double>(width() - 2 * CANVAS_PAD_X));
    double x_fraction = static_cast<double>(pos.x() - CANVAS_PAD_X) / inner_w;
    if (x_fraction < 0.0 || x_fraction > 1.0) {
        return -1;
    }

    for (size_t idx = 0; idx < m_window->flame_spans.size(); idx++) {
        const ktrace_flame_span& span = m_window->flame_spans[idx];
        if (span.depth != depth) {
            continue;
        }

        if ((span.end_fraction - span.start_fraction) * inner_w < MIN_SPAN_DRAW_PX) {
            continue;
        }

        if (x_fraction >= span.start_fraction && x_fraction <= span.end_fraction) {
            return static_cast<int>(idx);
        }
    }

    return -1;
}

void flame_timeline_canvas::paint_timeline_band(QPainter& painter, qreal inner_w) {
    uint64_t t0 = m_model->session.start_ns;
    uint64_t t1 = std::max(t0 + 1, m_model->session.effective_end_ns);
    double total_ns = static_cast<double>(t1 - t0);

    QRectF bars_box(CANVAS_PAD_X, TIMELINE_TOP_Y, inner_w, TIMELINE_BARS_H);
    painter.fillRect(bars_box, token(FLAME_ROOT_STONE));

    if (m_frames && !m_frames->empty()) {
        uint64_t max_wall = FRAME_BUDGET_240HZ_NS * 2;
        for (const ktrace_thread_frame& frame : *m_frames) {
            max_wall = std::max(max_wall, frame.wall_ns);
        }

        for (const ktrace_thread_frame& frame : *m_frames) {
            uint64_t f0 = std::clamp(frame.start_ns, t0, t1);
            uint64_t f1 = std::clamp(frame.end_ns, t0, t1);
            qreal x0 = CANVAS_PAD_X + (static_cast<double>(f0 - t0) / total_ns) * inner_w;
            qreal x1 = CANVAS_PAD_X + (static_cast<double>(f1 - t0) / total_ns) * inner_w;
            qreal bar_w = std::max<qreal>(1.5, x1 - x0);

            double h_ratio = std::clamp(static_cast<double>(frame.wall_ns) / static_cast<double>(max_wall), 0.08, 1.0);
            qreal bar_h = h_ratio * (TIMELINE_BARS_H - 4);
            bool over_budget = frame.wall_ns > FRAME_BUDGET_240HZ_NS;
            painter.fillRect(QRectF(x0, TIMELINE_TOP_Y + TIMELINE_BARS_H - bar_h, bar_w, bar_h),
                             over_budget ? token(FLAME_HOT_COPPER) : token(STATE_RUNNING_FILL));
        }

        qreal budget_y = TIMELINE_TOP_Y + TIMELINE_BARS_H -
                         (static_cast<double>(FRAME_BUDGET_240HZ_NS) / static_cast<double>(max_wall)) *
                             (TIMELINE_BARS_H - 4);
        painter.setPen(QPen(token(STLXTHEME_LINE_STRONG), 1.0, Qt::DashLine));
        painter.drawLine(QPointF(CANVAS_PAD_X, budget_y), QPointF(CANVAS_PAD_X + inner_w, budget_y));
    } else if (m_bars && !m_bars->empty()) {
        qreal bucket_w = inner_w / static_cast<qreal>(m_bars->size());
        for (size_t idx = 0; idx < m_bars->size(); idx++) {
            const ktrace_timeline_bar& bar = (*m_bars)[idx];
            qreal bar_h = std::clamp(bar.busy_fraction, 0.04, 1.0) * (TIMELINE_BARS_H - 4);
            QColor col = blend_rgb(FLAME_FRAME_SAND, FLAME_HOT_COPPER, bar.busy_fraction * MAX_COPPER_BLEND);
            painter.fillRect(QRectF(CANVAS_PAD_X + idx * bucket_w, TIMELINE_TOP_Y + TIMELINE_BARS_H - bar_h,
                                    std::max<qreal>(1.0, bucket_w - 1.0), bar_h),
                             col);
        }
    }

    int lane_y = TIMELINE_TOP_Y + TIMELINE_BARS_H + STATE_LANE_GAP_Y;
    painter.fillRect(QRectF(CANVAS_PAD_X, lane_y, inner_w, STATE_LANE_H), token(STATE_SLEEPING_FILL));

    if (m_lane_states) {
        for (const ktrace_thread_state& state : *m_lane_states) {
            uint64_t s0 = std::clamp(state.start_ns, t0, t1);
            uint64_t s1 = std::clamp(*state.end_ns, t0, t1);
            qreal sx0 = CANVAS_PAD_X + (static_cast<double>(s0 - t0) / total_ns) * inner_w;
            qreal sx1 = CANVAS_PAD_X + (static_cast<double>(s1 - t0) / total_ns) * inner_w;
            if (sx1 - sx0 >= MIN_SPAN_DRAW_PX) {
                uint32_t tone = state.state == thread_state_kind::RUNNING ? STATE_RUNNING_FILL : STATE_RUNNABLE_FILL;
                painter.fillRect(QRectF(sx0, lane_y, sx1 - sx0, STATE_LANE_H), token(tone));
            }
        }
    }

    qreal zx0 = CANVAS_PAD_X + (static_cast<double>(std::clamp(m_zoom_start_ns, t0, t1) - t0) / total_ns) * inner_w;
    qreal zx1 = CANVAS_PAD_X + (static_cast<double>(std::clamp(m_zoom_end_ns, t0, t1) - t0) / total_ns) * inner_w;
    int band_h = TIMELINE_BARS_H + STATE_LANE_GAP_Y + STATE_LANE_H;

    QColor shade_outside = token(STLXTHEME_INK);
    shade_outside.setAlpha(OUTSIDE_SHADE_ALPHA);
    if (zx0 > CANVAS_PAD_X) {
        painter.fillRect(QRectF(CANVAS_PAD_X, TIMELINE_TOP_Y, zx0 - CANVAS_PAD_X, band_h), shade_outside);
    }

    if (zx1 < CANVAS_PAD_X + inner_w) {
        painter.fillRect(QRectF(zx1, TIMELINE_TOP_Y, CANVAS_PAD_X + inner_w - zx1, band_h), shade_outside);
    }

    painter.setPen(QPen(token(SIGNAL_AMBER), SELECTION_STROKE_PX));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(zx0, TIMELINE_TOP_Y, std::max<qreal>(2.0, zx1 - zx0), band_h));
    painter.fillRect(QRect(0, FLAME_TOP_OFFSET_Y - 8, width(), 1), token(STLXTHEME_LINE));
}

void flame_timeline_canvas::paint_flame_spans(QPainter& painter, qreal inner_w) {
    if (!m_window || m_window->flame_spans.empty()) {
        painter.setFont(ui_font(STLXTHEME_BODY_SIZE));
        painter.setPen(token(STLXTHEME_TEXT_DIM));
        QRect empty_box(CANVAS_PAD_X, FLAME_TOP_OFFSET_Y, static_cast<int>(inner_w),
                        std::max(32, height() - FLAME_TOP_OFFSET_Y));
        painter.drawText(empty_box, Qt::AlignCenter, QStringLiteral("No active spans in selected time window."));
        return;
    }

    QFont label_font = mono_font(STLXTHEME_CAPTION_SIZE);
    painter.setFont(label_font);
    QFontMetricsF metrics(label_font);
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (size_t idx = 0; idx < m_window->flame_spans.size(); idx++) {
        const ktrace_flame_span& span = m_window->flame_spans[idx];
        if (static_cast<int>(span.depth) < m_scroll_row_offset) {
            continue;
        }

        qreal x0 = CANVAS_PAD_X + span.start_fraction * inner_w;
        qreal raw_w = (span.end_fraction - span.start_fraction) * inner_w;
        if (raw_w < MIN_SPAN_DRAW_PX) {
            continue;
        }

        int row_y = FLAME_TOP_OFFSET_Y + (static_cast<int>(span.depth) - m_scroll_row_offset) * FLAME_ROW_STEP_Y;
        if (row_y > height()) {
            continue;
        }

        qreal draw_w = raw_w > 3.0 ? (raw_w - 1.0) : raw_w;
        QRectF bar_rect(x0, row_y, draw_w, FLAME_BAR_H);

        double self_ratio = span.total_ns > 0
                                ? static_cast<double>(span.self_ns) / static_cast<double>(span.total_ns)
                                : 0.0;
        uint32_t base_tone = span.depth == 0 ? FLAME_ROOT_STONE : FLAME_FRAME_SAND;
        QColor fill = blend_rgb(base_tone, FLAME_HOT_COPPER, self_ratio * MAX_COPPER_BLEND);

        bool is_selected = static_cast<int>(idx) == m_selected_span;
        bool is_hovered = static_cast<int>(idx) == m_hovered_span;
        if (is_selected) {
            painter.setPen(QPen(token(SIGNAL_AMBER), SELECTION_STROKE_PX));
        } else if (is_hovered) {
            painter.setPen(QPen(token(STLXTHEME_LINE_STRONG), 1.0));
        } else {
            painter.setPen(Qt::NoPen);
        }

        painter.setBrush(fill);
        painter.drawRoundedRect(bar_rect, BAR_CORNER_RADIUS, BAR_CORNER_RADIUS);

        if (draw_w >= MIN_SPAN_TEXT_PX) {
            QString text = QString::fromStdString(span.name);
            QString elided = metrics.elidedText(text, Qt::ElideRight, draw_w - 10.0);
            painter.setPen(is_selected ? token(STLXTHEME_TEXT) : token(FLAME_TEXT_SAND));
            painter.drawText(bar_rect.adjusted(5.0, 0.0, -5.0, 0.0), Qt::AlignLeft | Qt::AlignVCenter, elided);
        }
    }

    painter.setRenderHint(QPainter::Antialiasing, false);
}

void flame_timeline_canvas::paint_hover_card(QPainter& painter) {
    if (!m_window || m_hovered_span < 0 ||
        static_cast<size_t>(m_hovered_span) >= m_window->flame_spans.size()) {
        return;
    }

    const ktrace_flame_span& span = m_window->flame_spans[static_cast<size_t>(m_hovered_span)];
    double win_denom = m_window->window_ns > 0 ? static_cast<double>(m_window->window_ns) : 1.0;
    double self_pct = 100.0 * static_cast<double>(span.self_ns) / win_denom;

    QString line1 = QString::fromStdString(span.name);
    QString line2 = QStringLiteral("Self %1 (%2%)   Total %3   %4 calls")
                        .arg(qstring_duration_ns(span.self_ns),
                             QString::number(self_pct, 'f', 1),
                             qstring_duration_ns(span.total_ns))
                        .arg(span.call_count);

    QFont title_font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::DemiBold);
    QFont meta_font = mono_font(STLXTHEME_CAPTION_SIZE);
    int w1 = QFontMetrics(title_font).horizontalAdvance(line1);
    int w2 = QFontMetrics(meta_font).horizontalAdvance(line2);
    int card_w = std::clamp(std::max(w1, w2) + 20, 180, std::max(180, width() - 24));
    int card_h = 46;

    int cx = std::clamp(m_hover_pos.x() + 12, 8, std::max(8, width() - card_w - 8));
    int cy = std::clamp(m_hover_pos.y() + 14, 8, std::max(8, height() - card_h - 8));
    QRectF card(cx, cy, card_w, card_h);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(token(STLXTHEME_RIM), 1.0));
    painter.setBrush(token(STLXTHEME_MENU_BG));
    painter.drawRoundedRect(card, STLXTHEME_INNER_RADIUS, STLXTHEME_INNER_RADIUS);

    painter.setFont(title_font);
    painter.setPen(token(STLXTHEME_TEXT));
    painter.drawText(card.adjusted(10, 5, -10, -22), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(title_font).elidedText(line1, Qt::ElideMiddle, card_w - 20));

    painter.setFont(meta_font);
    painter.setPen(token(STLXTHEME_TEXT_DIM));
    painter.drawText(card.adjusted(10, 23, -10, -4), Qt::AlignLeft | Qt::AlignVCenter, line2);
    painter.setRenderHint(QPainter::Antialiasing, false);
}

void flame_timeline_canvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_OPAQUE_POCKET));

    if (!m_message.isEmpty() || !m_model) {
        painter.setFont(ui_font(STLXTHEME_BODY_SIZE));
        painter.setPen(token(STLXTHEME_TEXT_DIM));
        painter.drawText(rect(), Qt::AlignCenter,
                         m_message.isEmpty() ? QStringLiteral("Open or record a ktrace session.") : m_message);
        return;
    }

    qreal inner_w = std::max<qreal>(1.0, width() - 2 * CANVAS_PAD_X);
    paint_timeline_band(painter, inner_w);
    paint_flame_spans(painter, inner_w);
    paint_hover_card(painter);
}

flame_graph_view::flame_graph_view(QWidget* parent) : QWidget(parent) {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* header = new perf_strip_bar(HEADER_BAR_HEIGHT, false, true, this);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(HEADER_PADDING_X, 0, HEADER_PADDING_X, 1);
    header_layout->setSpacing(HEADER_GAP);

    m_header_label = new QLabel(header);
    m_header_label->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    m_header_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                      .arg(token(STLXTHEME_TEXT_SECONDARY).name(QColor::HexArgb)));
    header_layout->addWidget(m_header_label, 1);

    m_thread_button = new ui::ui_pill_button(QStringLiteral("All Threads"), header);
    m_thread_button->set_has_chevron(true);
    connect(m_thread_button, &QAbstractButton::clicked, this, [this] { open_thread_picker(); });
    header_layout->addWidget(m_thread_button);

    m_reset_zoom_button = new ui::ui_pill_button(QStringLiteral("Reset Zoom"), header);
    m_reset_zoom_button->setEnabled(false);
    connect(m_reset_zoom_button, &QAbstractButton::clicked, this, [this] { reset_zoom(); });
    header_layout->addWidget(m_reset_zoom_button);
    root_layout->addWidget(header);

    auto* body = new QWidget(this);
    auto* body_layout = new QHBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    m_canvas = new flame_timeline_canvas(body);
    m_vertical_scrollbar = new QScrollBar(Qt::Vertical, body);
    m_vertical_scrollbar->setVisible(false);

    connect(m_vertical_scrollbar, &QScrollBar::valueChanged, this, [this](int value) {
        m_canvas->set_scroll_row_offset(value);
    });

    m_canvas->on_zoom_dragged = [this](uint64_t lo, uint64_t hi) { set_zoom_window(lo, hi); };
    m_canvas->on_zoom_reset = [this] { reset_zoom(); };
    m_canvas->on_viewport_resized = [this] { sync_vertical_scrollbar(); };
    m_canvas->on_vertical_scroll = [this](int delta) {
        m_vertical_scrollbar->setValue(m_vertical_scrollbar->value() + delta);
    };

    m_canvas->on_wheel_zoom = [this](double anchor_fraction, double scale) {
        if (!m_has_model) {
            return;
        }

        uint64_t full_lo = m_model.session.start_ns;
        uint64_t full_hi = std::max(full_lo + MIN_ZOOM_WINDOW_NS, m_model.session.effective_end_ns);
        double cur_span = static_cast<double>(m_zoom_end_ns - m_zoom_start_ns);
        double next_span = std::clamp(cur_span * scale,
                                      static_cast<double>(MIN_ZOOM_WINDOW_NS),
                                      static_cast<double>(full_hi - full_lo));

        double anchor_ns = static_cast<double>(m_zoom_start_ns) + anchor_fraction * cur_span;
        double raw_lo = anchor_ns - anchor_fraction * next_span;
        double clamped_lo = std::clamp(raw_lo, static_cast<double>(full_lo),
                                       static_cast<double>(full_hi) - next_span);
        uint64_t new_lo = static_cast<uint64_t>(std::llround(clamped_lo));
        uint64_t new_hi = std::min(full_hi, new_lo + static_cast<uint64_t>(std::llround(next_span)));
        set_zoom_window(new_lo, std::max(new_lo + MIN_ZOOM_WINDOW_NS, new_hi));
    };

    m_canvas->on_span_clicked = [this](const ktrace_flame_span& span, bool is_double_click) {
        update_header_and_footer();
        if (on_span_activated) {
            on_span_activated(span, is_double_click);
        }
    };

    body_layout->addWidget(m_canvas, 1);
    body_layout->addWidget(m_vertical_scrollbar);
    root_layout->addWidget(body, 1);

    auto* footer = new perf_strip_bar(FOOTER_BAR_HEIGHT, true, false, this);
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(HEADER_PADDING_X, 1, HEADER_PADDING_X, 0);

    m_footer_label = new QLabel(footer);
    m_footer_label->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    m_footer_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                      .arg(token(STLXTHEME_TEXT).name(QColor::HexArgb)));
    footer_layout->addWidget(m_footer_label, 1);
    root_layout->addWidget(footer);
}

flame_graph_view::~flame_graph_view() {
    if (m_active_picker) {
        m_active_picker->close();
    }

    stop_decode();
}

void flame_graph_view::stop_decode() {
    if (m_decode_task) {
        m_decode_task->on_stdout = nullptr;
        m_decode_task->on_stderr = nullptr;
        m_decode_task->on_finished = nullptr;
        m_decode_task->stop();
        m_decode_task.reset();
    }

    cleanup_temp_db();
}

void flame_graph_view::cleanup_temp_db() {
    if (!m_temp_db_path.isEmpty()) {
        QByteArray utf8 = m_temp_db_path.toUtf8();
        ::unlink(utf8.constData());
        QByteArray partial = utf8 + ".partial";
        ::unlink(partial.constData());
        m_temp_db_path.clear();
    }
}

void flame_graph_view::open_trace_path(const QString& path) {
    stop_decode();
    m_trace_path = path;
    m_trace_title = QFileInfo(path).fileName();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_has_model = false;
        m_canvas->set_status_message(QStringLiteral("Cannot open %1").arg(m_trace_title));
        return;
    }

    QByteArray prefix = file.read(TRACE_MAGIC_READ_BYTES);
    file.close();

    std::span<const uint8_t> head_span(reinterpret_cast<const uint8_t*>(prefix.constData()),
                                       static_cast<size_t>(prefix.size()));
    if (is_ktrace_capture(head_span)) {
        start_ktrace_decode(path);
        return;
    }

    ktrace_profile_model loaded;
    std::string error;
    if (!load_ktrace_database(path.toStdString(), loaded, error)) {
        m_has_model = false;
        m_canvas->set_status_message(QString::fromStdString(error));
        m_header_label->setText(m_trace_title);
        return;
    }

    set_model(std::move(loaded), m_trace_title);
}

void flame_graph_view::start_ktrace_decode(const QString& raw_ktrace_path) {
    stop_decode();
    g_temp_db_counter++;
    m_temp_db_path = QStringLiteral("/tmp/sheen-ktrace-%1-%2.db")
                         .arg(::getpid())
                         .arg(g_temp_db_counter);
    m_decode_stderr.clear();
    m_has_model = false;

    m_header_label->setText(QStringLiteral("Decoding %1...").arg(m_trace_title));
    m_canvas->set_status_message(QStringLiteral("Running ktrace-decode on %1...").arg(m_trace_title));

    m_decode_task = std::make_unique<process_task>();
    m_decode_task->on_stderr = [this](QByteArrayView chunk) {
        m_decode_stderr.append(chunk.data(), chunk.size());
    };

    m_decode_task->on_finished = [this](const process_exit& result) {
        finish_ktrace_decode(result);
    };

    process_spec spec;
    spec.program = QStringLiteral("/bin/ktrace-decode");
    spec.arguments = { QFileInfo(raw_ktrace_path).absoluteFilePath(), QStringLiteral("-o"), m_temp_db_path };

    QString start_error;
    if (!m_decode_task->start(spec, &start_error)) {
        m_header_label->setText(QStringLiteral("%1   Decode failed").arg(m_trace_title));
        m_canvas->set_status_message(QStringLiteral("ktrace-decode unavailable: %1").arg(start_error));
        stop_decode();
    }
}

void flame_graph_view::finish_ktrace_decode(const process_exit& exit_result) {
    if (exit_result.exit_code != 0) {
        QString detail = QString::fromUtf8(m_decode_stderr).trimmed();
        m_header_label->setText(QStringLiteral("%1   Decode failed").arg(m_trace_title));
        m_canvas->set_status_message(detail.isEmpty() ? QStringLiteral("ktrace-decode failed") : detail);
        cleanup_temp_db();
        return;
    }

    ktrace_profile_model loaded;
    std::string error;
    bool ok = load_ktrace_database(m_temp_db_path.toStdString(), loaded, error);
    cleanup_temp_db();

    if (!ok) {
        m_header_label->setText(QStringLiteral("%1   Decode failed").arg(m_trace_title));
        m_canvas->set_status_message(QString::fromStdString(error));
        return;
    }

    set_model(std::move(loaded), m_trace_title);
}

uint32_t flame_graph_view::dominant_workload_tid() const {
    std::unordered_map<uint32_t, uint64_t> run_ns_by_tid;
    for (const ktrace_cpu_slice& slice : m_model.cpu_slices) {
        if (!slice.is_idle && slice.end_ns.has_value() && *slice.end_ns > slice.start_ns) {
            run_ns_by_tid[slice.tid] += (*slice.end_ns - slice.start_ns);
        }
    }

    uint64_t best_running = 0;
    uint32_t chosen_tid = 0;
    for (const ktrace_thread& thread : m_model.threads) {
        if (thread.kind == KTRACE_TASK_IDLE || thread.latest_name == "ktrace") {
            continue;
        }

        uint64_t run_sum = run_ns_by_tid[thread.tid];
        if (run_sum >= best_running) {
            best_running = run_sum;
            chosen_tid = thread.tid;
        }
    }

    return chosen_tid;
}

void flame_graph_view::set_model(ktrace_profile_model model, const QString& trace_title) {
    stop_decode();
    m_model = std::move(model);
    m_trace_title = trace_title;
    m_has_model = true;

    m_filter_tid.reset();
    m_exclude_recorder = true;
    m_lane_tid = dominant_workload_tid();
    m_thread_button->setText(QStringLiteral("All Threads"));

    m_zoom_start_ns = m_model.session.start_ns;
    m_zoom_end_ns = std::max(m_zoom_start_ns + 1, m_model.session.effective_end_ns);
    recompute_window();
}

void flame_graph_view::open_thread_picker() {
    if (!m_has_model) {
        return;
    }

    if (m_active_picker) {
        m_active_picker->close();
    }

    QList<picker_choice> choices;
    choices.append({ QStringLiteral("__all__"), QStringLiteral("All Threads (Workload)"),
                     QStringLiteral("Excludes ktrace recorder"), {}, false, false });

    for (const ktrace_thread& thread : m_model.threads) {
        if (thread.kind == KTRACE_TASK_IDLE) {
            continue;
        }

        QString label = thread.latest_name.empty()
                            ? QStringLiteral("tid %1").arg(thread.tid)
                            : QString::fromStdString(thread.latest_name);
        QString detail = QStringLiteral("tid %1").arg(thread.tid);
        if (thread.pid.has_value()) {
            detail += QStringLiteral("  pid %1").arg(*thread.pid);
        }

        choices.append({ QString::number(thread.tid), label, detail, {}, false, false });
    }

    auto* picker = new ui::ui_searchable_picker(m_thread_button, QStringLiteral("Filter threads..."), choices);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    m_active_picker = picker;
    m_thread_button->set_is_open(true);

    QPointer<flame_graph_view> self(this);
    picker->on_closed = [self] {
        if (self) {
            self->m_thread_button->set_is_open(false);
        }
    };

    picker->on_selected = [self](const QString& choice_id) {
        if (!self) {
            return;
        }

        if (choice_id == QLatin1String("__all__")) {
            self->m_filter_tid.reset();
            self->m_exclude_recorder = true;
            self->m_lane_tid = self->dominant_workload_tid();
            self->m_thread_button->setText(QStringLiteral("All Threads"));
        } else {
            uint32_t chosen_tid = choice_id.toUInt();
            self->m_filter_tid = chosen_tid;
            self->m_exclude_recorder = false;
            self->m_lane_tid = chosen_tid;
            self->m_thread_button->setText(
                QString::fromStdString(thread_name_at(self->m_model, chosen_tid, self->m_zoom_start_ns)));
        }

        self->recompute_window();
    };

    picker->open_below_anchor();
}

void flame_graph_view::set_zoom_window(uint64_t start_ns, uint64_t end_ns) {
    if (!m_has_model || end_ns <= start_ns) {
        return;
    }

    m_zoom_start_ns = std::max(m_model.session.start_ns, start_ns);
    m_zoom_end_ns = std::min(std::max(m_model.session.start_ns + 1, m_model.session.effective_end_ns), end_ns);
    if (m_zoom_end_ns <= m_zoom_start_ns) {
        m_zoom_end_ns = m_zoom_start_ns + 1;
    }

    recompute_window();
}

void flame_graph_view::reset_zoom() {
    if (!m_has_model) {
        return;
    }

    m_zoom_start_ns = m_model.session.start_ns;
    m_zoom_end_ns = std::max(m_zoom_start_ns + 1, m_model.session.effective_end_ns);
    recompute_window();
}

void flame_graph_view::recompute_window() {
    if (!m_has_model) {
        return;
    }

    uint64_t full_start = m_model.session.start_ns;
    uint64_t full_end = std::max(full_start + 1, m_model.session.effective_end_ns);
    m_reset_zoom_button->setEnabled(m_zoom_start_ns > full_start || m_zoom_end_ns < full_end);

    ktrace_window_filter full_filter{ full_start, full_end, std::nullopt, m_filter_tid, m_exclude_recorder };
    m_timeline_bars = compute_ktrace_timeline_bars(m_model, full_filter, TIMELINE_BUCKET_COUNT);
    if (m_filter_tid.has_value()) {
        m_thread_frames = extract_ktrace_thread_frames(m_model, *m_filter_tid, full_start, full_end);
    } else {
        m_thread_frames.clear();
    }

    m_lane_states.clear();
    auto lane_it = std::lower_bound(m_model.thread_states.begin(), m_model.thread_states.end(), m_lane_tid,
                                    [](const ktrace_thread_state& state, uint32_t target) {
                                        return state.tid < target;
                                    });
    for (; lane_it != m_model.thread_states.end() && lane_it->tid == m_lane_tid; ++lane_it) {
        if (lane_it->state != thread_state_kind::SLEEPING &&
            lane_it->end_ns.has_value() && *lane_it->end_ns > lane_it->start_ns) {
            m_lane_states.push_back(*lane_it);
        }
    }

    ktrace_window_filter zoom_filter{ m_zoom_start_ns, m_zoom_end_ns, std::nullopt, m_filter_tid, m_exclude_recorder };
    m_window_view = query_ktrace_window(m_model, zoom_filter);

    m_max_flame_depth = 0;
    for (const ktrace_flame_span& span : m_window_view.flame_spans) {
        m_max_flame_depth = std::max(m_max_flame_depth, span.depth);
    }

    sync_vertical_scrollbar();
    m_canvas->set_render_state(&m_model, &m_window_view, &m_timeline_bars,
                               m_filter_tid.has_value() ? &m_thread_frames : nullptr,
                               &m_lane_states, m_zoom_start_ns, m_zoom_end_ns);
    update_header_and_footer();

    if (on_window_changed) {
        on_window_changed(m_model, m_window_view, m_filter_tid.has_value());
    }
}

void flame_graph_view::sync_vertical_scrollbar() {
    int total_rows = m_window_view.flame_spans.empty() ? 0 : static_cast<int>(m_max_flame_depth) + 1;
    int visible_rows = m_canvas->visible_flame_rows();
    int max_scroll = std::max(0, total_rows - visible_rows);

    m_vertical_scrollbar->setRange(0, max_scroll);
    m_vertical_scrollbar->setPageStep(visible_rows);
    m_vertical_scrollbar->setVisible(max_scroll > 0);
    if (m_vertical_scrollbar->value() > max_scroll) {
        m_vertical_scrollbar->setValue(max_scroll);
    }
}

void flame_graph_view::update_header_and_footer() {
    if (!m_has_model) {
        return;
    }

    double parallel_cpus = m_filter_tid.has_value()
                               ? 1.0
                               : static_cast<double>(std::max<uint32_t>(1, m_model.session.cpu_count));
    double denom = static_cast<double>(m_window_view.window_ns) * parallel_cpus;
    double busy_pct = denom > 0.0
                          ? std::min(100.0, 100.0 * static_cast<double>(m_window_view.busy_cpu_ns) / denom)
                          : 0.0;

    m_header_label->setText(
        QStringLiteral("%1   %2   %3 CPUs   %4 records   %5   Window %6 (%7% CPU)")
            .arg(m_trace_title, QString::fromStdString(m_model.session.arch))
            .arg(m_model.session.cpu_count)
            .arg(m_model.session.records)
            .arg(format_health_label(m_model),
                 qstring_duration_ns(m_window_view.window_ns),
                 QString::number(busy_pct, 'f', 1)));

    const ktrace_flame_span* sel = m_canvas->selected_span();
    if (sel) {
        QString loc = sel->source_file.empty()
                          ? QString()
                          : QStringLiteral("   %1:%2")
                                .arg(QString::fromStdString(sel->source_file))
                                .arg(sel->source_line);
        m_footer_label->setText(
            QStringLiteral("%1   Self %2   Total %3   %4 calls%5")
                .arg(QString::fromStdString(sel->name),
                     qstring_duration_ns(sel->self_ns),
                     qstring_duration_ns(sel->total_ns))
                .arg(sel->call_count)
                .arg(loc));
        return;
    }

    m_footer_label->setText(
        QStringLiteral("Running %1   Runnable %2   Sleeping %3   Syscalls %4")
            .arg(qstring_duration_ns(m_window_view.running_ns),
                 qstring_duration_ns(m_window_view.runnable_ns),
                 qstring_duration_ns(m_window_view.sleeping_ns))
            .arg(m_window_view.syscall_summaries.size()));
}
} // namespace sheen
