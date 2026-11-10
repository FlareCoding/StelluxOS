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
constexpr int BUTTON_GLYPH_GAP = 8;
constexpr int SMALL_BUTTON_GLYPH_GAP = 6;
constexpr int SMALL_BUTTON_GLYPH_SIZE = 14;

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

constexpr int CHIP_H = 20;
constexpr int CHIP_PADDING_X = 6;
constexpr int CHIP_LOCK_SIZE = 12;
constexpr int CHIP_LOCK_GAP = 5;
constexpr int MORE_BUTTON_SIZE = 28;

constexpr int LIST_ROW_H = 44;
constexpr int LIST_ROW_PADDING_LEFT = 8;
constexpr int LIST_ROW_PADDING_RIGHT = 6;
constexpr int LIST_ROW_GAP = 12;
constexpr int LIST_ICON_SIZE = 28;
constexpr int LIST_NAME_LINE = 18;
constexpr int LIST_BOX_PADDING = 4;
constexpr int LIST_SEPARATOR_INSET = 8;
constexpr int LIST_FOOTER_H = 44;
constexpr int LIST_FOOTER_PADDING_X = 10;

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

QString code_text(const QString& text) {
    QString family = QString::fromLatin1(STLXTHEME_MONO_FONT_FAMILY);
    QString color = theme_color(STLXTHEME_TEXT_SECONDARY).name();
    QString markup = QStringLiteral("<span style=\"font-family: '%1'; color: %2\">%3</span>");
    return markup.arg(family, color, text.toHtmlEscaped());
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

static QPainterPath circle_path(qreal x, qreal y, qreal radius) {
    QPainterPath path;
    path.addEllipse(QPointF(x, y), radius, radius);
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

static QPainterPath top_half_disc(qreal center_x, qreal center_y, qreal radius) {
    QPainterPath top = rect_path(center_x - radius, center_y - radius, 2 * radius, radius);
    return circle_path(center_x, center_y, radius).intersected(top);
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

static QPainterPath lock_path() {
    QPainterPath outer = rect_path(4.5, 5, 7, 1.5).united(top_half_disc(8, 5, 3.5));
    QPainterPath inner = rect_path(6.3, 5, 3.4, 1.5).united(top_half_disc(8, 5, 1.7));
    return rounded_rect(2.5, 6.5, 11, 8.5, 1.5).united(outer.subtracted(inner));
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
    case glyph::LOCK:
        return lock_path();
    case glyph::MORE:
        return circle_path(3, 8, 1.5).united(circle_path(8, 8, 1.5)).united(circle_path(13, 8, 1.5));
    case glyph::PLUS:
        return rect_path(7, 2.5, 2, 11).united(rect_path(2.5, 7, 11, 2));
    case glyph::PROGRAM: {
        QPainterPath prompt = polygon_path({
            { 3.6, 6.2 }, { 6.2, 8.2 }, { 3.6, 10.2 }, { 4.5, 11.3 }, { 8.5, 8.2 }, { 4.5, 5.1 }
        });
        return rounded_rect(1, 2, 14, 12, 1.5).subtracted(prompt).subtracted(rect_path(8.5, 10, 4, 1.4));
    }
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

void push_button::set_glyph(glyph shape) {
    m_glyph = shape;
    m_has_glyph = true;
    updateGeometry();
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

int push_button::glyph_size() const {
    return m_is_small ? SMALL_BUTTON_GLYPH_SIZE : STLXTHEME_GLYPH_SIZE;
}

/* The glyph and its gap before the text, or 0 without a glyph */
int push_button::glyph_width() const {
    if (!m_has_glyph) {
        return 0;
    }

    return glyph_size() + (m_is_small ? SMALL_BUTTON_GLYPH_GAP : BUTTON_GLYPH_GAP);
}

QSize push_button::sizeHint() const {
    int full_padding = m_is_primary ? STLXTHEME_ACCENT_BUTTON_PADDING_X : STLXTHEME_BUTTON_PADDING_X;
    int padding = m_is_small ? SMALL_BUTTON_PADDING_X : full_padding;
    int text_width = qCeil(QFontMetricsF(button_font(m_is_primary)).horizontalAdvance(text()));
    int height = m_is_small ? STLXTHEME_TOOLBAR_CONTROL_HEIGHT : STLXTHEME_CONTROL_HEIGHT;

    return QSize(2 * padding + glyph_width() + text_width, height);
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
    qreal content_width = glyph_width() + QFontMetricsF(font).horizontalAdvance(text());
    qreal left = (width() - content_width) / 2;

    if (m_has_glyph) {
        QColor glyph_color = isEnabled() && !is_accent ? theme_color(STLXTHEME_TEXT_SECONDARY) : colors.text;
        QRectF glyph_box(left, (height() - glyph_size()) / 2.0, glyph_size(), glyph_size());
        paint_glyph(painter, m_glyph, glyph_box, glyph_color);
    }

    QRectF text_box(left + glyph_width(), 0, content_width - glyph_width(), height());
    painter.setFont(font);
    painter.setPen(colors.text);
    painter.drawText(text_box, Qt::AlignVCenter, text());
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

chip::chip(const QString& text) : m_text(text) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize chip::sizeHint() const {
    qreal text_width = QFontMetricsF(mono_font(STLXTHEME_CAPTION_SIZE)).horizontalAdvance(m_text);
    int lock_width = CHIP_LOCK_SIZE + CHIP_LOCK_GAP;
    return QSize(2 * (CHIP_PADDING_X + CARD_BORDER) + lock_width + qCeil(text_width), CHIP_H);
}

void chip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    qreal radius = STLXTHEME_CHIP_RADIUS - 0.5;
    painter.setPen(QPen(theme_color(STLXTHEME_LINE), CARD_BORDER));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

    qreal lock_left = CHIP_PADDING_X + CARD_BORDER;
    QRectF lock(lock_left, (height() - CHIP_LOCK_SIZE) / 2.0, CHIP_LOCK_SIZE, CHIP_LOCK_SIZE);
    paint_glyph(painter, glyph::LOCK, lock, theme_color(STLXTHEME_TEXT_DIM));

    qreal left = lock.right() + CHIP_LOCK_GAP;
    painter.setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
    painter.drawText(QRectF(left, 0, width() - left, height()), Qt::AlignVCenter | Qt::AlignLeft, m_text);
}

more_button::more_button() {
    setFixedSize(MORE_BUTTON_SIZE, MORE_BUTTON_SIZE);
    setAttribute(Qt::WA_Hover);
}

/* Lit while the pointer is on its row, so the row shows where its menu opens */
void more_button::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    bool is_lit = underMouse() || (parentWidget() && parentWidget()->underMouse());

    if (is_lit) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme_color(STLXTHEME_SURFACE_HOVER));
        painter.drawRoundedRect(QRectF(rect()), STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
    }

    QRectF dots(0, 0, STLXTHEME_GLYPH_SIZE, STLXTHEME_GLYPH_SIZE);
    dots.moveCenter(QRectF(rect()).center());
    paint_glyph(painter, glyph::MORE, dots, theme_color(is_lit ? STLXTHEME_TEXT : STLXTHEME_TEXT_DIM));
}

void more_button::mouseReleaseEvent(QMouseEvent* event) {
    bool is_click = event->button() == Qt::LeftButton && rect().contains(event->position().toPoint());
    if (is_click && on_click) {
        on_click();
    }
}

list_row::list_row(const QString& name, const QString& path, const QString& args)
    : m_name(name), m_path(path), m_args(args), m_layout(new QHBoxLayout(this)) {
    setFixedHeight(LIST_ROW_H);
    m_layout->setContentsMargins(LIST_ROW_PADDING_LEFT, 0, LIST_ROW_PADDING_RIGHT, 0);
    m_layout->setSpacing(LIST_ROW_GAP);
    m_layout->addStretch(1);
}

void list_row::set_text(const QString& name, const QString& path, const QString& args) {
    m_name = name;
    m_path = path;
    m_args = args;
    update();
}

void list_row::set_icon(const QPixmap& icon) {
    m_icon = QPixmap();
    if (!icon.isNull()) {
        m_icon = icon.scaled(LIST_ICON_SIZE, LIST_ICON_SIZE, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    update();
}

void list_row::set_locked(bool is_locked) {
    m_is_locked = is_locked;
    update();
}

void list_row::set_open(bool is_open) {
    m_is_open = is_open;
    update();
}

void list_row::add_trailing(QWidget* widget) {
    m_layout->addWidget(widget, 0, Qt::AlignVCenter);
}

void list_row::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    qreal center_y = height() / 2.0;

    if (is_lit()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme_color(STLXTHEME_SURFACE_HOVER));
        painter.drawRoundedRect(QRectF(rect()), STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
    }

    QRectF icon(LIST_ROW_PADDING_LEFT, center_y - LIST_ICON_SIZE / 2.0, LIST_ICON_SIZE, LIST_ICON_SIZE);
    if (!m_icon.isNull()) {
        painter.drawPixmap(icon.topLeft(), m_icon);
    } else {
        qreal radius = STLXTHEME_CONTROL_RADIUS - 0.5;
        painter.setPen(QPen(theme_color(STLXTHEME_LINE_SUBTLE), CARD_BORDER));
        painter.setBrush(theme_color(STLXTHEME_SURFACE_HOVER));
        painter.drawRoundedRect(icon.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

        QRectF glyph_box(0, 0, STLXTHEME_GLYPH_SIZE, STLXTHEME_GLYPH_SIZE);
        glyph_box.moveCenter(icon.center());
        paint_glyph(painter, glyph::PROGRAM, glyph_box, theme_color(STLXTHEME_TEXT_SECONDARY));
    }

    /* The text runs up to the first trailing widget */
    qreal text_left = icon.right() + LIST_ROW_GAP;
    qreal text_right = m_layout->count() > 1 ? m_layout->itemAt(1)->geometry().left() - LIST_ROW_GAP : width();
    qreal text_width = qMax(0.0, text_right - text_left);
    QFont name_font = ui_font(STLXTHEME_BODY_SIZE, QFont::Medium);
    QFont path_font = mono_font(STLXTHEME_CAPTION_SIZE);

    painter.setFont(name_font);
    painter.setPen(theme_color(m_is_locked ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT));
    QString name = QFontMetricsF(name_font).elidedText(m_name, Qt::ElideRight, text_width);
    painter.drawText(QRectF(text_left, center_y - LIST_NAME_LINE, text_width, LIST_NAME_LINE), Qt::AlignVCenter, name);

    QFontMetricsF path_metrics(path_font);
    QString path = path_metrics.elidedText(m_path, Qt::ElideMiddle, text_width);
    QRectF path_line(text_left, center_y, text_width, CAPTION_LINE_HEIGHT);
    painter.setFont(path_font);
    painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
    painter.drawText(path_line, Qt::AlignVCenter | Qt::AlignLeft, path);

    qreal args_left = text_left + path_metrics.horizontalAdvance(path + QLatin1Char(' '));
    QString args = path_metrics.elidedText(m_args, Qt::ElideRight, qMax(0.0, text_right - args_left));
    QRectF args_line(args_left, path_line.top(), text_right - args_left, CAPTION_LINE_HEIGHT);
    painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
    painter.drawText(args_line, Qt::AlignVCenter | Qt::AlignLeft, args);
}

void list_row::mouseReleaseEvent(QMouseEvent* event) {
    bool is_click = event->button() == Qt::LeftButton && rect().contains(event->position().toPoint());
    if (is_click && on_click) {
        on_click();
    }
}

/* The list draws no hairline above or below a lit row, so the pointer entering or leaving a row repaints the list */
void list_row::enterEvent(QEnterEvent*) {
    parentWidget()->update();
}

void list_row::leaveEvent(QEvent*) {
    parentWidget()->update();
}

list_box::list_box() : m_entries(new QVBoxLayout) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_entries->setContentsMargins(LIST_BOX_PADDING, LIST_BOX_PADDING, LIST_BOX_PADDING, LIST_BOX_PADDING);
    m_entries->setSpacing(0);
    layout->addLayout(m_entries);
}

void list_box::add_row(list_row* row) {
    m_rows.append(row);
    m_entries->addWidget(row);
}

void list_box::add_entry(QWidget* entry) {
    m_entries->addWidget(entry);
}

void list_box::set_footer(push_button* add, const QString& hint) {
    m_footer = new QWidget;
    m_footer->setFixedHeight(LIST_FOOTER_H);
    auto* layout = new QHBoxLayout(m_footer);
    layout->setContentsMargins(LIST_FOOTER_PADDING_X, 0, LIST_FOOTER_PADDING_X, 0);

    QLabel* text = make_text(hint, ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal), STLXTHEME_TEXT_DIM);
    text->setTextFormat(Qt::RichText);
    layout->addWidget(add);
    layout->addStretch(1);
    layout->addWidget(text);

    static_cast<QVBoxLayout*>(this->layout())->addWidget(m_footer);
}

void list_box::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QColor line = theme_color(STLXTHEME_LINE_SUBTLE);
    list_row* previous = nullptr;

    for (int i = 0; i < m_entries->count(); i++) {
        QWidget* entry = m_entries->itemAt(i)->widget();
        list_row* row = m_rows.contains(entry) ? static_cast<list_row*>(entry) : nullptr;
        bool is_row_pair = row && previous;
        bool is_pair_unlit = is_row_pair && !row->is_lit() && !previous->is_lit();
        if (is_pair_unlit) {
            QRect place = row->geometry();
            int hairline_width = place.width() - 2 * LIST_SEPARATOR_INSET;
            painter.fillRect(QRect(place.left() + LIST_SEPARATOR_INSET, place.top() - 1, hairline_width, 1), line);
        }

        previous = row;
    }

    if (m_footer) {
        painter.fillRect(QRect(0, m_footer->y(), width(), 1), line);
    }
}
