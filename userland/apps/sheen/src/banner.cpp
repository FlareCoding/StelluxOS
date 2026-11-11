#include "banner.h"
#include "chrome.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QtMath>

static constexpr int BANNER_HEIGHT = 40;
static constexpr int BANNER_INSET_X = 16;
static constexpr int GLYPH_SIZE = STLXTHEME_GLYPH_SIZE;
static constexpr int GLYPH_TEXT_GAP = 10;
static constexpr int ACTION_GAP = 8;
static constexpr int ACTION_HEIGHT = 28;
static constexpr int CLOSE_SIZE = 20;
static constexpr qreal FACE_ALPHA = 0.03;
static constexpr qreal GLYPH_STROKE = 1.5;
static constexpr qreal CLOSE_HALF = 3;
static constexpr qreal GLYPH_RING_INSET = 1.5;
static constexpr qreal MARK_TOP = -3.5;
static constexpr qreal MARK_BOTTOM = 0.5;
static constexpr qreal MARK_DOT = 3.5;
static constexpr qreal ARROW_SIZE = 3.5;
static constexpr int RELOAD_ARC_START = 60;
static constexpr int RELOAD_ARC_SPAN = 300;
static constexpr int ARC_UNITS_PER_DEGREE = 16;

namespace {

class close_glyph : public QAbstractButton {
public:
    explicit close_glyph(QWidget* parent) : QAbstractButton(parent) {
        setFixedSize(CLOSE_SIZE, CLOSE_SIZE);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        setToolTip(QStringLiteral("Dismiss"));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(STLXTHEME_SURFACE_HOVER));
            painter.drawRoundedRect(rect(), STLXTHEME_INNER_RADIUS, STLXTHEME_INNER_RADIUS);
        }

        QPointF center = QRectF(rect()).center();
        painter.setPen(QPen(token(underMouse() ? STLXTHEME_TEXT : STLXTHEME_TEXT_DIM), GLYPH_STROKE, Qt::SolidLine,
                            Qt::RoundCap));
        painter.drawLine(center + QPointF(-CLOSE_HALF, -CLOSE_HALF), center + QPointF(CLOSE_HALF, CLOSE_HALF));
        painter.drawLine(center + QPointF(CLOSE_HALF, -CLOSE_HALF), center + QPointF(-CLOSE_HALF, CLOSE_HALF));
    }
};

} // namespace

banner::banner(QWidget* parent) : QWidget(parent), m_text(new QLabel(this)), m_close(new close_glyph(this)) {
    setFixedHeight(BANNER_HEIGHT);
    m_text->setFont(ui_font(STLXTHEME_BODY_SIZE));
    m_text->setTextFormat(Qt::RichText);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(BANNER_INSET_X + GLYPH_SIZE + GLYPH_TEXT_GAP, 0, BANNER_INSET_X, 1);
    layout->setSpacing(ACTION_GAP);
    layout->addWidget(m_text, 1);

    m_actions = new QHBoxLayout;
    m_actions->setSpacing(ACTION_GAP);
    layout->addLayout(m_actions);
    layout->addWidget(m_close);

    connect(static_cast<QAbstractButton*>(m_close), &QAbstractButton::clicked, this, [this] {
        if (on_dismissed) {
            on_dismissed();
        }
    });
    hide();
}

void banner::show_message(const banner_message& message) {
    m_kind = message.kind;
    QString subject = message.subject.toHtmlEscaped();
    m_text->setText(QStringLiteral("<span style=\"color:%1\">%2</span><span style=\"color:%3\">%4</span>")
                        .arg(token(STLXTHEME_TEXT).name(), subject, token(STLXTHEME_TEXT_SECONDARY).name(),
                             message.text.toHtmlEscaped()));

    while (QLayoutItem* item = m_actions->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const banner_action& action : message.actions) {
        auto* button = new QPushButton(action.label, this);
        button->setFixedHeight(ACTION_HEIGHT);
        button->setFocusPolicy(Qt::NoFocus);
        std::function<void()> run = action.run;
        connect(button, &QPushButton::clicked, this, [run] { run(); });
        m_actions->addWidget(button);
    }

    m_close->setVisible(message.kind == banner_kind::CHANGE);
    update();
}

// A failure carries the danger color and every other banner the notice tone
void banner::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QColor face = token(STLXTHEME_GLASS_LIGHT);
    face.setAlphaF(FACE_ALPHA);
    painter.fillRect(rect(), face);
    painter.fillRect(QRect(0, height() - 1, width(), 1), token(STLXTHEME_LINE));

    QRectF glyph(BANNER_INSET_X, (height() - GLYPH_SIZE) / 2.0, GLYPH_SIZE, GLYPH_SIZE);
    QRectF ring = glyph.adjusted(GLYPH_RING_INSET, GLYPH_RING_INSET, -GLYPH_RING_INSET, -GLYPH_RING_INSET);
    QColor color = token(m_kind == banner_kind::FAILURE ? STLXTHEME_DANGER : NOTICE_TONE);

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, GLYPH_STROKE, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    if (m_kind == banner_kind::FAILURE) {
        QPointF center = glyph.center();
        painter.drawEllipse(ring);
        painter.drawLine(center + QPointF(0, MARK_TOP), center + QPointF(0, MARK_BOTTOM));
        painter.drawPoint(center + QPointF(0, MARK_DOT));
        return;
    }

    // An open circle with an arrowhead where it starts, the sign for a reload
    qreal angle = qDegreesToRadians(static_cast<qreal>(RELOAD_ARC_START));
    QPointF tip = ring.center() + QPointF(qCos(angle), -qSin(angle)) * (ring.width() / 2);
    painter.drawArc(ring, RELOAD_ARC_START * ARC_UNITS_PER_DEGREE, RELOAD_ARC_SPAN * ARC_UNITS_PER_DEGREE);
    painter.drawLine(tip, tip + QPointF(-ARROW_SIZE, 0));
    painter.drawLine(tip, tip + QPointF(0, ARROW_SIZE));
}
