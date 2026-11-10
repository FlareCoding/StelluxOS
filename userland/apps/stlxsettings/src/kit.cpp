#include "kit.hpp"

#include <stlxtheme/theme.h>

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
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

constexpr int KEYCAPS_W = 150;
constexpr int KEYCAP_H = 22;
constexpr int KEYCAP_PADDING_X = 6;
constexpr int KEYCAP_GAP = 4;
constexpr int KEYCAP_SHADOW = 1;

constexpr int VALUE_LABEL_W = 64;
constexpr int VALUE_UNIT_GAP = 4;
constexpr qreal DISABLED_OPACITY = 0.38;

constexpr int KEY_HINT_PADDING_X = 5;
constexpr const char* FIND_HINT = "Ctrl F";
constexpr int SEARCH_PADDING_LEFT = 9;
constexpr int SEARCH_PADDING_RIGHT = 5;
constexpr int SEARCH_GAP = 8;
constexpr int SEARCH_CLEAR_SIZE = 20;
constexpr int SEARCH_CLEAR_GLYPH_SIZE = 12;

constexpr int RESULT_PADDING_LEFT = 14;
constexpr int RESULT_PADDING_RIGHT = 12;
constexpr int RESULT_GAP = 16;
constexpr int RESULT_VALUE_W = 72;
constexpr qreal RESULT_MARK_PADDING = 1;
constexpr qreal RESULT_MARK_RADIUS = 2;

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

static QPainterPath search_path() {
    QPainterPath lens;
    lens.addEllipse(QPointF(6.8, 6.8), 4.6, 4.6);

    QPainterPath handle(QPointF(10.3, 10.3));
    handle.lineTo(14, 14);

    return stroked(lens).united(stroked(handle));
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

static QPainterPath document_path() {
    QPainterPath page(QPointF(3.5, 1));
    page.lineTo(9, 1);
    page.lineTo(13, 5);

    page.lineTo(13, 13.5);
    page.arcTo(QRectF(10, 12, 3, 3), 0, -90);
    page.lineTo(3.5, 15);

    page.arcTo(QRectF(2, 12, 3, 3), 270, -90);
    page.lineTo(2, 2.5);
    page.arcTo(QRectF(2, 1, 3, 3), 180, -90);
    page.closeSubpath();

    return page.subtracted(rect_path(5, 8, 6, 1.5)).subtracted(rect_path(5, 11, 4.5, 1.5));
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
    case glyph::INFO:
        return circle_path(8, 8, 7).subtracted(rect_path(7, 7, 2, 5)).subtracted(rect_path(7, 3.5, 2, 2));
    case glyph::SEARCH:
        return search_path();
    case glyph::LOCK:
        return lock_path();
    case glyph::MORE:
        return circle_path(3, 8, 1.5).united(circle_path(8, 8, 1.5)).united(circle_path(13, 8, 1.5));
    case glyph::PLUS:
        return rect_path(7, 2.5, 2, 11).united(rect_path(2.5, 7, 11, 2));
    case glyph::CHEVRON:
        return polygon_path({ { 6, 3.2 }, { 10.8, 8 }, { 6, 12.8 }, { 4.8, 11.6 }, { 8.4, 8 }, { 4.8, 4.4 } });
    case glyph::CLOSE:
        return polygon_path({
            { 4.2, 2.8 }, { 8, 6.6 }, { 11.8, 2.8 }, { 13.2, 4.2 }, { 9.4, 8 }, { 13.2, 11.8 },
            { 11.8, 13.2 }, { 8, 9.4 }, { 4.2, 13.2 }, { 2.8, 11.8 }, { 6.6, 8 }, { 2.8, 4.2 }
        });
    case glyph::DOCUMENT:
        return document_path();
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

section_head::section_head(const QString& title, const QString& caption)
    : m_title(title.toUpper()), m_caption(caption) {
    setFixedHeight(SECTION_GAP_ABOVE + CAPTION_LINE_HEIGHT + SECTION_GAP_BELOW);
}

void section_head::set_title(const QString& title) {
    m_title = title.toUpper();
    update();
}

void section_head::set_caption(const QString& caption) {
    m_caption = caption;
    update();
}

void section_head::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QFont title_font = caps_font();
    QFont caption_font = mono_font(STLXTHEME_CAPTION_SIZE);
    QFontMetricsF title_metrics(title_font);
    qreal baseline = SECTION_GAP_ABOVE + (CAPTION_LINE_HEIGHT - title_metrics.height()) / 2 + title_metrics.ascent();

    painter.setFont(title_font);
    painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
    painter.drawText(QPointF(0, baseline), m_title);

    qreal caption_width = QFontMetricsF(caption_font).horizontalAdvance(m_caption);
    painter.setFont(caption_font);
    painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
    painter.drawText(QPointF(width() - caption_width, baseline), m_caption);
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
    setObjectName(label);
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

void toggle::set_on(bool is_on) {
    m_is_on = is_on;
    update();
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

void list_row::set_leading(QWidget* widget) {
    m_leading = widget;
    m_layout->insertWidget(0, widget, 0, Qt::AlignVCenter);
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
    if (m_leading) {
        icon = QRectF(m_leading->geometry());
    } else if (!m_icon.isNull()) {
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

    /* The text runs up to the first trailing widget, which follows the stretch */
    int stretch_index = m_leading ? 1 : 0;
    qreal text_left = icon.right() + LIST_ROW_GAP;
    bool has_trailing = m_layout->count() > stretch_index + 1;
    qreal text_right = has_trailing ? m_layout->itemAt(stretch_index + 1)->geometry().left() - LIST_ROW_GAP : width();
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

static qreal keycap_width(const QFontMetricsF& metrics, const QString& key) {
    return qMax<qreal>(KEYCAP_H, qCeil(metrics.horizontalAdvance(key)) + 2 * KEYCAP_PADDING_X);
}

keycaps::keycaps(const QString& chord) {
    setFixedHeight(KEYCAP_H + KEYCAP_SHADOW);
    set_chord(chord);
}

/* Keys read as printed on a keyboard: ctrl+alt+t shows Ctrl, Alt and T */
void keycaps::set_chord(const QString& chord) {
    m_keys.clear();
    for (const QString& key : chord.split(QLatin1Char('+'), Qt::SkipEmptyParts)) {
        QString trimmed = key.trimmed();
        m_keys.append(trimmed.left(1).toUpper() + trimmed.mid(1).toLower());
    }

    QFontMetricsF metrics(mono_font(STLXTHEME_CAPTION_SIZE));
    qreal caps_width = 0;
    for (const QString& key : m_keys) {
        caps_width += keycap_width(metrics, key) + KEYCAP_GAP;
    }

    /* Chords that fit `KEYCAPS_W` share that width, so the names after them line up */
    setFixedWidth(qMax(KEYCAPS_W, qCeil(caps_width - KEYCAP_GAP)));
    update();
}

void keycaps::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font = mono_font(STLXTHEME_CAPTION_SIZE);
    QFontMetricsF metrics(font);
    painter.setFont(font);
    qreal left = 0;

    for (const QString& key : m_keys) {
        qreal cap_width = keycap_width(metrics, key);
        QRectF cap(left + 0.5, 0.5, cap_width - 1, KEYCAP_H - 1);

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme_color(STLXTHEME_KEYCAP_SHADOW));
        painter.drawRoundedRect(cap.translated(0, KEYCAP_SHADOW), STLXTHEME_CHIP_RADIUS, STLXTHEME_CHIP_RADIUS);

        painter.setPen(QPen(theme_color(STLXTHEME_LINE), CARD_BORDER));
        painter.setBrush(theme_color(STLXTHEME_KEYCAP_FACE));
        painter.drawRoundedRect(cap, STLXTHEME_CHIP_RADIUS - 0.5, STLXTHEME_CHIP_RADIUS - 0.5);

        painter.setPen(theme_color(STLXTHEME_TEXT));
        painter.drawText(cap, Qt::AlignCenter, key);
        left += cap_width + KEYCAP_GAP;
    }
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

qreal key_hint_width(const QString& key) {
    qreal text_width = QFontMetricsF(mono_font(STLXTHEME_CAPTION_SIZE)).horizontalAdvance(key);
    return qCeil(text_width) + 2 * (KEY_HINT_PADDING_X + CARD_BORDER);
}

void paint_key_hint(QPainter& painter, const QPointF& top_left, const QString& key) {
    QRectF box(top_left, QSizeF(key_hint_width(key), KEY_HINT_H));
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(QPen(theme_color(STLXTHEME_LINE), CARD_BORDER));
    painter.setBrush(theme_color(STLXTHEME_KEY_HINT_FACE));
    painter.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), STLXTHEME_CHIP_RADIUS - 0.5,
                            STLXTHEME_CHIP_RADIUS - 0.5);

    painter.setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    painter.setPen(theme_color(STLXTHEME_TEXT_DIM));
    painter.drawText(box, Qt::AlignCenter, key);
    painter.restore();
}

/* The line edit paints only the text, and the field paints its frame, lens, hint and clear button around it */
search_field::search_field() : m_line(new QLineEdit) {
    setFixedHeight(STLXTHEME_CONTROL_HEIGHT);
    m_line->setFrame(false);
    m_line->setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::Normal));
    m_line->setPlaceholderText(QStringLiteral("Search settings"));
    m_line->installEventFilter(this);

    QPalette palette = m_line->palette();
    palette.setColor(QPalette::Base, Qt::transparent);
    palette.setColor(QPalette::Text, theme_color(STLXTHEME_TEXT));
    palette.setColor(QPalette::PlaceholderText, theme_color(STLXTHEME_TEXT_DIM));
    m_line->setPalette(palette);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(SEARCH_PADDING_LEFT + STLXTHEME_GLYPH_SIZE + SEARCH_GAP, 0,
                               SEARCH_PADDING_RIGHT + SEARCH_CLEAR_SIZE + SEARCH_GAP, 0);
    layout->addWidget(m_line);

    QObject::connect(m_line, &QLineEdit::textChanged, [this](const QString& text) {
        update();

        if (on_edit) {
            on_edit(text.simplified());
        }
    });
}

QString search_field::query() const {
    return m_line->text().simplified();
}

void search_field::focus_query() {
    m_line->setFocus(Qt::ShortcutFocusReason);
    m_line->selectAll();
}

void search_field::clear_query() {
    m_line->clear();
}

QRectF search_field::clear_rect() const {
    return QRectF(width() - SEARCH_PADDING_RIGHT - SEARCH_CLEAR_SIZE, (height() - SEARCH_CLEAR_SIZE) / 2.0,
                  SEARCH_CLEAR_SIZE, SEARCH_CLEAR_SIZE);
}

void search_field::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    bool is_focused = m_line->hasFocus();
    qreal inset = CARD_BORDER / 2.0;

    painter.setPen(QPen(theme_color(is_focused ? STLXTHEME_LINE_STRONG : STLXTHEME_LINE), CARD_BORDER));
    painter.setBrush(theme_color(STLXTHEME_RECESS));
    painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), STLXTHEME_CONTROL_RADIUS - inset,
                            STLXTHEME_CONTROL_RADIUS - inset);

    if (is_focused) {
        QPainterPath outline;
        outline.addRoundedRect(QRectF(rect()), STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
        painter.setClipPath(outline);
        painter.fillRect(QRectF(0, height() - STLXTHEME_FOCUS_RULE_WIDTH, width(), STLXTHEME_FOCUS_RULE_WIDTH),
                         theme_color(STLXTHEME_ACCENT));
        painter.setClipping(false);
    }

    QRectF lens(SEARCH_PADDING_LEFT, (height() - STLXTHEME_GLYPH_SIZE) / 2.0, STLXTHEME_GLYPH_SIZE,
                STLXTHEME_GLYPH_SIZE);
    paint_glyph(painter, glyph::SEARCH, lens, theme_color(STLXTHEME_TEXT_DIM));

    if (m_line->text().isEmpty()) {
        QString hint = QLatin1String(FIND_HINT);
        QPointF hint_corner(width() - SEARCH_PADDING_RIGHT - key_hint_width(hint), (height() - KEY_HINT_H) / 2.0);
        paint_key_hint(painter, hint_corner, hint);
        return;
    }

    QRectF clear = clear_rect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme_color(STLXTHEME_SEARCH_CLEAR_FACE));
    painter.drawRoundedRect(clear, STLXTHEME_INNER_RADIUS, STLXTHEME_INNER_RADIUS);

    QRectF cross(clear.center().x() - SEARCH_CLEAR_GLYPH_SIZE / 2.0, clear.center().y() - SEARCH_CLEAR_GLYPH_SIZE / 2.0,
                 SEARCH_CLEAR_GLYPH_SIZE, SEARCH_CLEAR_GLYPH_SIZE);
    paint_glyph(painter, glyph::CLOSE, cross, theme_color(STLXTHEME_TEXT_SECONDARY));
}

void search_field::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_line->text().isEmpty() && clear_rect().contains(event->position())) {
        m_line->clear();
    }

    m_line->setFocus(Qt::MouseFocusReason);
}

bool search_field::eventFilter(QObject*, QEvent* event) {
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
        update();
        return false;
    }

    if (event->type() != QEvent::KeyPress || !on_navigation_key) {
        return false;
    }

    int key = static_cast<QKeyEvent*>(event)->key();
    bool is_navigation = key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Return || key == Qt::Key_Enter ||
                         key == Qt::Key_Escape;
    if (is_navigation) {
        on_navigation_key(key);
    }

    return is_navigation;
}

result_row::result_row(const QString& label, const QString& place, const QString& key, const QString& value,
                       const QString& query)
    : m_label(label), m_place(place), m_key(key), m_value(value), m_query(query) {
    setFixedHeight(STLXTHEME_SETTING_ROW_HEIGHT);
    setAttribute(Qt::WA_Hover);
}

void result_row::set_chosen(bool is_chosen) {
    m_is_chosen = is_chosen;
    update();
}

/* Draws `text` from the line's left, the first match of the query marked behind it */
void result_row::paint_marked_text(QPainter& painter, const QRectF& line, const QString& text, const QFont& font,
                                   uint32_t color) const {
    QFontMetricsF metrics(font);
    QString shown = metrics.elidedText(text, Qt::ElideRight, line.width());
    qsizetype match = shown.indexOf(m_query, 0, Qt::CaseInsensitive);

    if (match >= 0 && !m_query.isEmpty()) {
        qreal mark_left = line.left() + metrics.horizontalAdvance(shown.left(match)) - RESULT_MARK_PADDING;
        qreal mark_width = metrics.horizontalAdvance(shown.mid(match, m_query.size())) + 2 * RESULT_MARK_PADDING;
        qreal mark_height = metrics.ascent() + metrics.descent();
        QRectF mark(mark_left, line.center().y() - mark_height / 2, mark_width, mark_height);

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme_color(STLXTHEME_MATCH_MARK));
        painter.drawRoundedRect(mark, RESULT_MARK_RADIUS, RESULT_MARK_RADIUS);
        painter.setPen(QPen(theme_color(STLXTHEME_RIM), CARD_BORDER));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(mark.adjusted(-0.5, -0.5, 0.5, 0.5), RESULT_MARK_RADIUS + 0.5,
                                RESULT_MARK_RADIUS + 0.5);
    }

    painter.setFont(font);
    painter.setPen(theme_color(color));
    painter.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, shown);
}

void result_row::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    bool is_lit = m_is_chosen || underMouse();

    /* The fill keeps within the card's rounded inside, so a lit first or last row leaves its corners round */
    if (is_lit) {
        QRectF inside = QRectF(parentWidget()->rect()).adjusted(CARD_BORDER, CARD_BORDER, -CARD_BORDER, -CARD_BORDER);
        QPainterPath card_inside;
        card_inside.addRoundedRect(inside.translated(-QPointF(pos())), STLXTHEME_CARD_RADIUS - CARD_BORDER,
                                   STLXTHEME_CARD_RADIUS - CARD_BORDER);
        painter.fillPath(card_inside, theme_color(STLXTHEME_SURFACE_HOVER));
    }

    QRectF chevron(width() - RESULT_PADDING_RIGHT - STLXTHEME_GLYPH_SIZE, (height() - STLXTHEME_GLYPH_SIZE) / 2.0,
                   STLXTHEME_GLYPH_SIZE, STLXTHEME_GLYPH_SIZE);
    paint_glyph(painter, glyph::CHEVRON, chevron, theme_color(m_is_chosen ? STLXTHEME_TEXT : STLXTHEME_TEXT_DIM));

    QFont value_font = mono_font(STLXTHEME_BODY_SIZE);
    qreal value_advance = QFontMetricsF(value_font).horizontalAdvance(m_value);
    qreal value_width = m_value.isEmpty() ? 0 : qMax<qreal>(RESULT_VALUE_W, value_advance);
    qreal value_left = chevron.left() - RESULT_GAP - value_width;

    painter.setFont(value_font);
    painter.setPen(theme_color(STLXTHEME_TEXT));
    painter.drawText(QRectF(value_left, 0, value_width, height()), Qt::AlignVCenter | Qt::AlignRight, m_value);

    QFont key_font = mono_font(STLXTHEME_CAPTION_SIZE);
    qreal key_right = m_value.isEmpty() ? chevron.left() - RESULT_GAP : value_left - RESULT_GAP;
    qreal key_width = qMin(QFontMetricsF(key_font).horizontalAdvance(m_key), key_right / 2);
    QRectF key_line(key_right - key_width, 0, key_width, height());
    paint_marked_text(painter, key_line, m_key, key_font, STLXTHEME_TEXT_DIM);

    qreal label_width = key_right - key_width - RESULT_GAP - RESULT_PADDING_LEFT;
    qreal label_top = (height() - BODY_LINE_HEIGHT - CAPTION_LINE_HEIGHT) / 2.0;
    QRectF label_line(RESULT_PADDING_LEFT, label_top, label_width, BODY_LINE_HEIGHT);
    paint_marked_text(painter, label_line, m_label, ui_font(STLXTHEME_BODY_SIZE, QFont::Medium), STLXTHEME_TEXT);

    QFont place_font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::Normal);
    QString place = QFontMetricsF(place_font).elidedText(m_place, Qt::ElideRight, label_width);
    painter.setFont(place_font);
    painter.setPen(theme_color(STLXTHEME_TEXT_SECONDARY));
    painter.drawText(QRectF(RESULT_PADDING_LEFT, label_line.bottom(), label_width, CAPTION_LINE_HEIGHT),
                     Qt::AlignVCenter | Qt::AlignLeft, place);
}

void result_row::mouseReleaseEvent(QMouseEvent* event) {
    if (rect().contains(event->position().toPoint()) && on_click) {
        on_click();
    }
}
