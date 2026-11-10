#include "kit.hpp"

#include <stlxtheme/theme.h>

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QtMath>

constexpr qreal CAPS_TRACKING_EM = 0.06;
constexpr qreal GLYPH_DESIGN_SIZE = 16;
constexpr qreal GLYPH_STROKE_WIDTH = 2;

constexpr int SECTION_GAP_ABOVE = 20;
constexpr int SECTION_GAP_BELOW = 8;

constexpr int CARD_BORDER = 1;
constexpr int SEPARATOR_INSET = 12;
constexpr int ROW_PADDING_LEFT = 14;
constexpr int ROW_PADDING_RIGHT = 12;
constexpr int ROW_SPACING = 16;

constexpr int SMALL_BUTTON_PADDING_X = 10;

constexpr int SEGMENT_TRACK_INSET = 2;
constexpr int SEGMENT_GAP = 2;
constexpr int SEGMENT_PADDING_X = 11;
constexpr int SEGMENT_MARK_SIZE = 10;
constexpr int SEGMENT_MARK_GAP = 7;

constexpr int TOGGLE_W = 34;
constexpr int TOGGLE_H = 20;
constexpr int TOGGLE_KNOB_SIZE = 14;
constexpr int TOGGLE_KNOB_INSET = 3;

constexpr int SLIDER_W = 200;
constexpr int SLIDER_H = 26;
constexpr int SLIDER_TRACK_H = 4;
constexpr int SLIDER_TICK_TOP = 5;
constexpr int SLIDER_TICK_H = 4;
constexpr int SLIDER_KNOB_SIZE = 16;
constexpr int SLIDER_HALO = 5;

constexpr int VALUE_LABEL_W = 64;
constexpr int VALUE_UNIT_GAP = 4;
constexpr qreal DISABLED_OPACITY = 0.38;

static QPainterPath g_glyph_paths[GLYPH_COUNT];

namespace {

struct button_colors {
    QColor fill;
    QColor line;
    QColor text;
};

class separator : public QWidget {
public:
    separator() {
        setFixedHeight(1);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(
            rect().adjusted(SEPARATOR_INSET, 0, -SEPARATOR_INSET, 0),
            QColor::fromRgba(STLXTHEME_LINE_SUBTLE)
        );
    }
};

} // namespace

QColor theme_color(uint32_t argb) {
    return QColor::fromRgba(argb);
}

/* Scaled from the desktop's body font, so libstlxqpa's theme stays the one owner of the pixel to point conversion */
static QFont font_at_em(const char* family, float em_px) {
    QFont font = QGuiApplication::font();
    font.setFamily(QString::fromLatin1(family));
    font.setPointSizeF(font.pointSizeF() * em_px / STLXTHEME_BODY_SIZE);
    return font;
}

QFont ui_font(float em_px, QFont::Weight weight) {
    QFont font = font_at_em(STLXTHEME_UI_FONT_FAMILY, em_px);
    font.setWeight(weight);
    return font;
}

QFont mono_font(float em_px) {
    QFont font = font_at_em(STLXTHEME_MONO_FONT_FAMILY, em_px);
    font.setStyleHint(QFont::Monospace);
    return font;
}

QFont caps_font() {
    QFont font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, CAPS_TRACKING_EM * STLXTHEME_CAPTION_SIZE);
    return font;
}

QLabel* make_text(const QString& text, const QFont& font, uint32_t color) {
    auto* label = new QLabel(text);
    label->setFont(font);

    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, theme_color(color));
    label->setPalette(palette);

    return label;
}

static QPainterPath rounded_rect(qreal x, qreal y, qreal width, qreal height, qreal radius) {
    QPainterPath path;
    path.addRoundedRect(QRectF(x, y, width, height), radius, radius);
    return path;
}

static QPainterPath rect_path(qreal x, qreal y, qreal width, qreal height) {
    QPainterPath path;
    path.addRect(QRectF(x, y, width, height));
    return path;
}

static QPainterPath polygon_path(std::initializer_list<QPointF> points) {
    QPainterPath path;
    path.addPolygon(QPolygonF(QList<QPointF>(points)));
    path.closeSubpath();
    return path;
}

/* Glyphs drawn as lines become filled outlines, so every glyph paints with one fill */
static QPainterPath stroked(const QPainterPath& line) {
    QPainterPathStroker stroker;
    stroker.setWidth(GLYPH_STROKE_WIDTH);
    stroker.setCapStyle(Qt::RoundCap);
    return stroker.createStroke(line);
}

/* A quarter disc whose corner sits at the center, swept from `start_degrees` */
static QPainterPath quarter_disc(qreal center_x, qreal center_y, qreal radius, qreal start_degrees) {
    QPainterPath path(QPointF(center_x, center_y));
    path.arcTo(QRectF(center_x - radius, center_y - radius, 2 * radius, 2 * radius), start_degrees, 90);
    path.closeSubpath();
    return path;
}

static QPainterPath power_path() {
    QPainterPath arc;
    QRectF circle(3, 3.27, 10, 10);
    arc.arcMoveTo(circle, 132.85);
    arc.arcTo(circle, 132.85, 274.3);

    QPainterPath stem(QPointF(8, 1.8));
    stem.lineTo(8, 6.8);

    return stroked(arc).united(stroked(stem));
}

static QPainterPath keyboard_path() {
    const QRectF keys[] = {
        { 2, 6, 2, 1 }, { 5, 6, 2, 1 }, { 8, 6, 2, 1 }, { 11, 6, 2, 1 }, { 12, 6, 2, 3 }, { 2, 8, 2, 1 },
        { 5, 8, 2, 1 }, { 8, 8, 2, 1 }, { 11, 8, 2, 1 }, { 2, 10, 2, 1 }, { 5, 10, 6, 1 }, { 12, 10, 2, 1 },
    };

    QPainterPath holes;
    for (const QRectF& key : keys) {
        holes.addRect(key);
    }

    return rounded_rect(0, 4, 16, 9, 2).subtracted(holes.simplified());
}

static QPainterPath build_glyph_path(glyph shape) {
    switch (shape) {
    case glyph::WALLPAPER: {
        QPainterPath star = polygon_path({
            { 7, 4.5 }, { 7.9, 7.6 }, { 11, 8.5 }, { 7.9, 9.4 }, { 7, 12.5 }, { 6.1, 9.4 }, { 3, 8.5 }, { 6.1, 7.6 }
        });
        return rounded_rect(1, 2, 14, 12, 2).subtracted(star).subtracted(rect_path(11, 4, 2, 2));
    }
    case glyph::DOCK:
        return rounded_rect(1, 2, 14, 12, 2.5).subtracted(rect_path(5, 10, 6, 2));
    case glyph::DISPLAY:
        return rounded_rect(1, 2, 14, 9, 1.5).united(rect_path(7, 11, 2, 2)).united(rounded_rect(4, 13, 8, 2, 1));
    case glyph::KEYBOARD:
        return keyboard_path();
    case glyph::POWER:
        return power_path();
    case glyph::LOGO:
        return quarter_disc(7, 7, 7, 90).united(quarter_disc(9, 9, 7, 270));
    }

    return QPainterPath();
}

void paint_glyph(QPainter& painter, glyph shape, const QRectF& box, const QColor& color) {
    QPainterPath& path = g_glyph_paths[static_cast<int>(shape)];
    if (path.isEmpty()) {
        path = build_glyph_path(shape);
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(box.topLeft());
    painter.scale(box.width() / GLYPH_DESIGN_SIZE, box.height() / GLYPH_DESIGN_SIZE);

    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(path);
    painter.restore();
}

section_head::section_head(const QString& title, const QString& config_section)
    : m_title(title.toUpper()), m_config_section(config_section) {
    setFixedHeight(SECTION_GAP_ABOVE + CAPTION_LINE_HEIGHT + SECTION_GAP_BELOW);
}

void section_head::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QFont title_font = caps_font();
    QFont section_font = mono_font(STLXTHEME_CAPTION_SIZE);
    QFontMetricsF title_metrics(title_font);
    qreal baseline = SECTION_GAP_ABOVE + (CAPTION_LINE_HEIGHT - title_metrics.height()) / 2 + title_metrics.ascent();

    painter.setFont(title_font);
    painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
    painter.drawText(QPointF(0, baseline), m_title);

    qreal section_width = QFontMetricsF(section_font).horizontalAdvance(m_config_section);
    painter.setFont(section_font);
    painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
    painter.drawText(QPointF(width() - section_width, baseline), m_config_section);
}

card::card() : m_layout(new QVBoxLayout(this)) {
    m_layout->setContentsMargins(CARD_BORDER, CARD_BORDER, CARD_BORDER, CARD_BORDER);
    m_layout->setSpacing(0);
}

void card::add_row(QWidget* row) {
    if (m_layout->count() > 0) {
        m_layout->addWidget(new separator);
    }

    m_layout->addWidget(row);
}

/* The border is drawn over a fill covering the whole card, and its 1 px line sits on the pixel grid */
void card::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme_color(STLXTHEME_CARD));
    painter.drawRoundedRect(QRectF(rect()), STLXTHEME_CARD_RADIUS, STLXTHEME_CARD_RADIUS);

    qreal inset = CARD_BORDER / 2.0;
    qreal radius = STLXTHEME_CARD_RADIUS - inset;
    painter.setPen(QPen(theme_color(STLXTHEME_CARD_LINE), CARD_BORDER));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), radius, radius);
}

setting_label::setting_label(const QString& label, const QString& key)
    : m_key(make_text(key, mono_font(STLXTHEME_CAPTION_SIZE), STLXTHEME_TEXT_DIM)) {
    QLabel* name = make_text(label, ui_font(STLXTHEME_BODY_SIZE, QFont::Medium), STLXTHEME_TEXT);
    name->setFixedHeight(BODY_LINE_HEIGHT);
    m_key->setFixedHeight(CAPTION_LINE_HEIGHT);
    m_key->setVisible(!key.isEmpty());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(name);
    layout->addWidget(m_key);
}

void setting_label::set_key(const QString& key) {
    m_key->setText(key);
    m_key->setVisible(!key.isEmpty());
}

setting_row::setting_row(const QString& label, const QString& key)
    : m_label(new setting_label(label, key)), m_layout(new QHBoxLayout(this)) {
    setFixedHeight(STLXTHEME_SETTING_ROW_HEIGHT);
    m_layout->setContentsMargins(ROW_PADDING_LEFT, 0, ROW_PADDING_RIGHT, 0);
    m_layout->setSpacing(ROW_SPACING);
    m_layout->addWidget(m_label, 1, Qt::AlignVCenter);
}

void setting_row::add_control(QWidget* control) {
    m_layout->addWidget(control, 0, Qt::AlignVCenter);
}

/* A plain button's hover and press never take amber, which marks state only. Disabled buttons drop the fill */
static button_colors colors_of_button(
    bool is_enabled,
    bool is_accent,
    bool is_hovered,
    bool is_pressed,
    bool has_keyboard_focus
) {
    if (!is_enabled) {
        return { Qt::transparent, theme_color(STLXTHEME_LINE_SUBTLE), theme_color(STLXTHEME_TEXT_DISABLED) };
    }

    if (is_accent) {
        uint32_t fill = is_pressed ? STLXTHEME_ACCENT_PRESS : is_hovered ? STLXTHEME_ACCENT_HOVER : STLXTHEME_ACCENT;
        QColor line = has_keyboard_focus ? theme_color(STLXTHEME_TEXT) : QColor(Qt::transparent);
        return { theme_color(fill), line, theme_color(STLXTHEME_ACCENT_TEXT) };
    }

    uint32_t fill = is_pressed ? STLXTHEME_SURFACE_PRESS : is_hovered ? STLXTHEME_SURFACE_HOVER : STLXTHEME_SURFACE;
    bool is_lit = !is_pressed && (is_hovered || has_keyboard_focus);
    uint32_t line = is_lit ? STLXTHEME_LINE_STRONG : STLXTHEME_LINE;

    return { theme_color(fill), theme_color(line), theme_color(STLXTHEME_TEXT) };
}

push_button::push_button(const QString& text) {
    setText(text);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    connect(this, &QAbstractButton::clicked, this, [this] {
        if (on_click) {
            on_click();
        }
    });
}

void push_button::set_primary(bool is_primary) {
    m_is_primary = is_primary;
    updateGeometry();
    update();
}

void push_button::set_small(bool is_small) {
    m_is_small = is_small;
    updateGeometry();
}

static QFont button_font(bool is_accent) {
    return ui_font(STLXTHEME_BODY_SIZE, is_accent ? QFont::DemiBold : QFont::Medium);
}

QSize push_button::sizeHint() const {
    int full_padding = m_is_primary ? STLXTHEME_ACCENT_BUTTON_PADDING_X : STLXTHEME_BUTTON_PADDING_X;
    int padding = m_is_small ? SMALL_BUTTON_PADDING_X : full_padding;
    int text_width = qCeil(QFontMetricsF(button_font(m_is_primary)).horizontalAdvance(text()));
    int height = m_is_small ? STLXTHEME_TOOLBAR_CONTROL_HEIGHT : STLXTHEME_CONTROL_HEIGHT;

    return QSize(2 * padding + text_width, height);
}

void push_button::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    bool is_accent = m_is_primary && isEnabled();
    bool has_keyboard_focus = hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange);
    button_colors colors = colors_of_button(isEnabled(), is_accent, underMouse(), isDown(), has_keyboard_focus);

    qreal inset = CARD_BORDER / 2.0;
    qreal radius = STLXTHEME_CONTROL_RADIUS - inset;
    painter.setPen(QPen(colors.line, CARD_BORDER));
    painter.setBrush(colors.fill);
    painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), radius, radius);

    QFont font = button_font(is_accent);
    qreal text_width = QFontMetricsF(font).horizontalAdvance(text());
    qreal left = (width() - text_width) / 2;

    painter.setFont(font);
    painter.setPen(colors.text);
    painter.drawText(QRectF(left, 0, text_width, height()), Qt::AlignVCenter, text());
}

segmented::segmented(const QStringList& labels, int chosen, mark_painter paint_mark)
    : m_labels(labels), m_paint_mark(std::move(paint_mark)), m_chosen(chosen) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

qreal segmented::segment_width(int index) const {
    qreal text_width = QFontMetricsF(ui_font(STLXTHEME_BODY_SIZE, QFont::Medium)).horizontalAdvance(m_labels[index]);
    return 2 * SEGMENT_PADDING_X + SEGMENT_MARK_SIZE + SEGMENT_MARK_GAP + qCeil(text_width);
}

QRectF segmented::segment_rect(int index) const {
    qreal left = SEGMENT_TRACK_INSET;
    for (int i = 0; i < index; i++) {
        left += segment_width(i) + SEGMENT_GAP;
    }

    return QRectF(left, SEGMENT_TRACK_INSET, segment_width(index), height() - 2 * SEGMENT_TRACK_INSET);
}

int segmented::segment_at(const QPointF& position) const {
    for (int i = 0; i < m_labels.size(); i++) {
        if (segment_rect(i).contains(position)) {
            return i;
        }
    }

    return -1;
}

QSize segmented::sizeHint() const {
    qreal width = 2 * SEGMENT_TRACK_INSET + SEGMENT_GAP * (m_labels.size() - 1);
    for (int i = 0; i < m_labels.size(); i++) {
        width += segment_width(i);
    }

    return QSize(qCeil(width), STLXTHEME_CONTROL_HEIGHT);
}

void segmented::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    qreal inset = CARD_BORDER / 2.0;
    qreal track_radius = STLXTHEME_CONTROL_RADIUS - inset;
    qreal segment_radius = STLXTHEME_INNER_RADIUS - inset;

    painter.setPen(QPen(theme_color(STLXTHEME_LINE), CARD_BORDER));
    painter.setBrush(theme_color(STLXTHEME_RECESS));
    painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), track_radius, track_radius);

    painter.setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::Medium));
    for (int i = 0; i < m_labels.size(); i++) {
        QRectF segment = segment_rect(i);
        bool is_chosen = i == m_chosen;
        bool is_lit = is_chosen || i == m_hovered;

        if (is_lit) {
            QPen ring = is_chosen ? QPen(theme_color(STLXTHEME_LINE_ON), CARD_BORDER) : QPen(Qt::NoPen);
            painter.setPen(ring);
            painter.setBrush(theme_color(is_chosen ? STLXTHEME_SURFACE_ON : STLXTHEME_SURFACE_HOVER));
            painter.drawRoundedRect(segment.adjusted(inset, inset, -inset, -inset), segment_radius, segment_radius);
        }

        QColor text = theme_color(is_lit ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY);
        qreal mark_left = segment.left() + SEGMENT_PADDING_X;
        qreal text_left = mark_left + SEGMENT_MARK_SIZE + SEGMENT_MARK_GAP;
        QRectF mark(mark_left, segment.center().y() - SEGMENT_MARK_SIZE / 2.0, SEGMENT_MARK_SIZE, SEGMENT_MARK_SIZE);

        m_paint_mark(painter, mark, i, text);

        painter.setPen(text);
        painter.drawText(
            QRectF(text_left, segment.top(), segment.right() - text_left, segment.height()),
            Qt::AlignVCenter | Qt::AlignLeft,
            m_labels[i]
        );
    }
}

void segmented::mouseMoveEvent(QMouseEvent* event) {
    int hovered = segment_at(event->position());
    if (hovered != m_hovered) {
        m_hovered = hovered;
        update();
    }
}

void segmented::mousePressEvent(QMouseEvent* event) {
    int pressed = segment_at(event->position());
    bool is_left_press = event->button() == Qt::LeftButton;
    bool is_new_choice = pressed >= 0 && pressed != m_chosen;
    if (!is_left_press || !is_new_choice) {
        return;
    }

    m_chosen = pressed;
    update();

    if (on_choose) {
        on_choose(pressed);
    }
}

void segmented::leaveEvent(QEvent*) {
    m_hovered = -1;
    update();
}

toggle::toggle(bool is_on) : m_is_on(is_on) {
    setFixedSize(TOGGLE_W, TOGGLE_H);
    setAttribute(Qt::WA_Hover);
}

void toggle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    bool is_hovered = underMouse();
    qreal radius = TOGGLE_H / 2.0;

    if (m_is_on) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme_color(is_hovered ? STLXTHEME_SWITCH_HOVER_TRACK : STLXTHEME_TEXT_SECONDARY));
    } else {
        uint32_t ring = is_hovered ? STLXTHEME_SWITCH_HOVER_RING : STLXTHEME_LINE_STRONG;
        painter.setPen(QPen(theme_color(ring), CARD_BORDER));
        painter.setBrush(theme_color(STLXTHEME_RECESS));
    }

    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5, radius - 0.5);

    qreal knob_left = m_is_on ? TOGGLE_W - TOGGLE_KNOB_INSET - TOGGLE_KNOB_SIZE : TOGGLE_KNOB_INSET;
    QRectF knob(knob_left, TOGGLE_KNOB_INSET, TOGGLE_KNOB_SIZE, TOGGLE_KNOB_SIZE);
    painter.setPen(Qt::NoPen);
    if (!m_is_on) {
        painter.setBrush(theme_color(STLXTHEME_KNOB_SHADOW));
        painter.drawEllipse(knob.translated(0, 1));
    }

    painter.setBrush(theme_color(m_is_on ? STLXTHEME_SWITCH_KNOB : STLXTHEME_TEXT_DIM));
    painter.drawEllipse(knob);
}

void toggle::mouseReleaseEvent(QMouseEvent* event) {
    bool is_click = event->button() == Qt::LeftButton && rect().contains(event->position().toPoint());
    if (!is_click) {
        return;
    }

    m_is_on = !m_is_on;
    update();

    if (on_toggle) {
        on_toggle(m_is_on);
    }
}

slider::slider(int minimum, int maximum, int step, int value, int tick_count)
    : m_minimum(minimum), m_maximum(maximum), m_step(step), m_value(qBound(minimum, value, maximum)),
      m_tick_count(tick_count) {
    setFixedSize(SLIDER_W, SLIDER_H);
    setAttribute(Qt::WA_Hover);
}

void slider::set_value(int value) {
    m_value = qBound(m_minimum, value, m_maximum);
    update();
}

/* The knob's center stays a radius inside each end, so it never leaves the track */
qreal slider::x_of(int value) const {
    qreal fraction = static_cast<qreal>(value - m_minimum) / (m_maximum - m_minimum);
    return SLIDER_KNOB_SIZE / 2.0 + fraction * (width() - SLIDER_KNOB_SIZE);
}

void slider::move_to(qreal x) {
    qreal fraction = qBound(0.0, (x - SLIDER_KNOB_SIZE / 2.0) / (width() - SLIDER_KNOB_SIZE), 1.0);
    int steps = qRound(fraction * (m_maximum - m_minimum) / m_step);
    int value = qMin(m_maximum, m_minimum + steps * m_step);
    if (value == m_value) {
        return;
    }

    m_value = value;
    update();

    if (on_change) {
        on_change(value);
    }
}

void slider::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(isEnabled() ? 1 : DISABLED_OPACITY);
    qreal center_y = height() / 2.0;
    QRectF track(0.5, center_y - SLIDER_TRACK_H / 2.0 + 0.5, width() - 1, SLIDER_TRACK_H - 1);
    qreal track_radius = SLIDER_TRACK_H / 2.0;

    painter.setPen(QPen(theme_color(STLXTHEME_LINE), CARD_BORDER));
    painter.setBrush(theme_color(STLXTHEME_RECESS));
    painter.drawRoundedRect(track, track_radius, track_radius);

    qreal knob_x = x_of(m_value);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme_color(STLXTHEME_TEXT_DIM));
    painter.drawRoundedRect(QRectF(0, track.top() - 0.5, knob_x, SLIDER_TRACK_H), track_radius, track_radius);

    for (int i = 0; i < m_tick_count; i++) {
        qreal x = qRound(SLIDER_KNOB_SIZE / 2.0 + i * (width() - SLIDER_KNOB_SIZE) / (m_tick_count - 1.0));
        painter.fillRect(QRectF(x, center_y + SLIDER_TICK_TOP, 1, SLIDER_TICK_H), theme_color(STLXTHEME_LINE_STRONG));
    }

    bool is_lit = isEnabled() && (underMouse() || m_is_dragging);
    QPointF knob(knob_x, center_y);
    qreal knob_radius = SLIDER_KNOB_SIZE / 2.0;
    if (is_lit) {
        painter.setBrush(theme_color(STLXTHEME_KNOB_HALO));
        painter.drawEllipse(knob, knob_radius + SLIDER_HALO, knob_radius + SLIDER_HALO);
    }

    painter.setBrush(theme_color(STLXTHEME_KNOB_SHADOW));
    painter.drawEllipse(knob + QPointF(0, 1), knob_radius + 0.5, knob_radius + 0.5);
    painter.setBrush(theme_color(is_lit ? STLXTHEME_TEXT : STLXTHEME_SLIDER_KNOB));
    painter.drawEllipse(knob, knob_radius, knob_radius);
}

void slider::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }

    m_is_dragging = true;
    move_to(event->position().x());
    update();
}

void slider::mouseMoveEvent(QMouseEvent* event) {
    if (m_is_dragging) {
        move_to(event->position().x());
    }
}

void slider::mouseReleaseEvent(QMouseEvent*) {
    m_is_dragging = false;
    update();
}

value_label::value_label(const QString& value, const QString& unit) : m_value(value), m_unit(unit) {
    setFixedHeight(STLXTHEME_CONTROL_HEIGHT);
}

void value_label::set_value(const QString& value) {
    m_value = value;
    update();
}

QSize value_label::sizeHint() const {
    return QSize(VALUE_LABEL_W, STLXTHEME_CONTROL_HEIGHT);
}

void value_label::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setOpacity(isEnabled() ? 1 : DISABLED_OPACITY);

    QFont value_font = mono_font(STLXTHEME_BODY_SIZE);
    QFont unit_font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal);
    QFontMetricsF value_metrics(value_font);
    qreal unit_width = QFontMetricsF(unit_font).horizontalAdvance(m_unit) + VALUE_UNIT_GAP;
    qreal baseline = (height() - value_metrics.height()) / 2 + value_metrics.ascent();

    painter.setFont(value_font);
    painter.setPen(theme_color(STLXTHEME_TEXT));
    painter.drawText(QPointF(width() - unit_width - value_metrics.horizontalAdvance(m_value), baseline), m_value);

    painter.setFont(unit_font);
    painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
    painter.drawText(QPointF(width() - unit_width + VALUE_UNIT_GAP, baseline), m_unit);
}
