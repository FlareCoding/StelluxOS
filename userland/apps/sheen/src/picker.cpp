#include "picker.h"
#include "chrome.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

static constexpr int PICKER_WIDTH = 560;
static constexpr int PICKER_TOP = 8;
static constexpr int PANEL_PADDING = 8;
static constexpr int FIELD_HEIGHT = 36;
static constexpr int FIELD_PADDING_X = 12;
static constexpr int PREFIX_GAP = 6;
static constexpr int HELP_HEIGHT = 30;
static constexpr int UNDERLINE_HEIGHT = 2;
static constexpr qreal HALF_PIXEL = 0.5;

namespace {

/* The recessed query row, underlined in amber since it always holds the focus while the picker is open */
class query_row : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        painter.setPen(QPen(token(STLXTHEME_LINE_STRONG), 1));
        painter.setBrush(token(STLXTHEME_RECESS));
        painter.drawRoundedRect(edge, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL);

        QPainterPath shape;
        shape.addRoundedRect(QRectF(rect()), STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
        painter.setClipPath(shape);
        painter.fillRect(QRectF(0, height() - UNDERLINE_HEIGHT, width(), UNDERLINE_HEIGHT), token(STLXTHEME_ACCENT));
    }
};

} // namespace

picker::picker(QWidget* parent)
    : QWidget(parent), m_prefix(new QLabel), m_query(new QLineEdit), m_help(new QLabel), m_aside(new QLabel) {
    setFixedWidth(PICKER_WIDTH);
    auto* row = new query_row(this);
    row->setFixedHeight(FIELD_HEIGHT);

    m_prefix->setParent(row);
    m_prefix->setFont(mono_font(STLXTHEME_BODY_SIZE));
    m_prefix->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT_DIM).name()));

    m_query->setParent(row);
    m_query->setFrame(false);
    m_query->setFont(mono_font(STLXTHEME_BODY_SIZE));
    m_query->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; color: %1; }")
                               .arg(token(STLXTHEME_TEXT).name()));

    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(FIELD_PADDING_X, 0, FIELD_PADDING_X, 0);
    row_layout->setSpacing(PREFIX_GAP);
    row_layout->addWidget(m_prefix);
    row_layout->addWidget(m_query, 1);

    auto* help_row = new QWidget(this);
    help_row->setFixedHeight(HELP_HEIGHT);
    for (QLabel* label : { m_help, m_aside }) {
        label->setParent(help_row);
        label->setFont(ui_font(STLXTHEME_CAPTION_SIZE));
        label->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT_DIM).name()));
        label->setTextFormat(Qt::RichText);
    }

    auto* help_layout = new QHBoxLayout(help_row);
    help_layout->setContentsMargins(FIELD_PADDING_X, 0, FIELD_PADDING_X, 0);
    help_layout->addWidget(m_help, 1);
    help_layout->addWidget(m_aside);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(PANEL_PADDING, PANEL_PADDING, PANEL_PADDING, 0);
    layout->setSpacing(0);
    layout->addWidget(row);
    layout->addWidget(help_row);

    connect(m_query, &QLineEdit::textEdited, this, [this](const QString& query) {
        if (on_changed) {
            on_changed(query);
        }
    });

    m_query->installEventFilter(this);
    hide();
}

void picker::open(const QString& prefix, const QString& help, const QString& aside) {
    m_prefix->setText(prefix);
    m_query->clear();
    m_aside->setText(aside);
    set_help(help);
    adjustSize();

    move((parentWidget()->width() - width()) / 2, PICKER_TOP);
    raise();
    show();
    m_query->setFocus();
}

void picker::set_help(const QString& help) {
    m_help->setText(help);
}

QString picker::emphasized(const QString& text) {
    return QStringLiteral("<span style=\"color: %1; font-weight: 500\">%2</span>")
        .arg(token(STLXTHEME_TEXT_SECONDARY).name(), text.toHtmlEscaped());
}

QString picker::code(const QString& text) {
    QString family = QString::fromLatin1(STLXTHEME_MONO_FONT_FAMILY);
    return QStringLiteral("<span style=\"color: %1; font-family: '%2'\">%3</span>")
        .arg(token(STLXTHEME_TEXT_SECONDARY).name(), family, text.toHtmlEscaped());
}

void picker::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    painter.setPen(QPen(token(STLXTHEME_RIM), 1));
    painter.setBrush(token(STLXTHEME_MENU_BG));
    painter.drawRoundedRect(edge, STLXTHEME_MENU_RADIUS - HALF_PIXEL, STLXTHEME_MENU_RADIUS - HALF_PIXEL);
}

// Enter accepts and Esc cancels, and the focus leaving cancels too, as a click back in the text does
bool picker::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_query && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Escape) {
            finish(key->key() != Qt::Key_Escape);
            return true;
        }
    }

    if (watched == m_query && event->type() == QEvent::FocusOut && isVisible()) {
        finish(false);
    }

    return QWidget::eventFilter(watched, event);
}

// Hidden first, so the focus it gives back finishes nothing twice
void picker::finish(bool is_accepted) {
    hide();
    if (is_accepted && on_accepted) {
        on_accepted(m_query->text());
    } else if (!is_accepted && on_cancelled) {
        on_cancelled();
    }
}
