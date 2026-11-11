#include "placeholder.h"
#include "chrome.h"
#include "encoding.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

static constexpr int GLYPH_WIDTH = 36;
static constexpr int GLYPH_HEIGHT = 44;
static constexpr int GLYPH_FOLD = 10;
static constexpr int TEXT_WIDTH = 460;
static constexpr int SECTION_GAP = 12;
static constexpr int BUTTON_GAP = 8;
static constexpr float TITLE_SIZE = 15.5f;
static constexpr qreal GLYPH_STROKE = 1.5;
static constexpr QByteArrayView LATIN_1 = "ISO-8859-1";

namespace {

/* A page with its top right corner folded, the sign for a document */
class document_glyph : public QWidget {
public:
    explicit document_glyph(QWidget* parent) : QWidget(parent) {
        setFixedSize(GLYPH_WIDTH, GLYPH_HEIGHT);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(token(STLXTHEME_TEXT_DIM), GLYPH_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        QRectF page = QRectF(rect()).adjusted(GLYPH_STROKE, GLYPH_STROKE, -GLYPH_STROKE, -GLYPH_STROKE);
        const QPointF outline[] = {
            page.topLeft(),
            QPointF(page.right() - GLYPH_FOLD, page.top()),
            QPointF(page.right(), page.top() + GLYPH_FOLD),
            page.bottomRight(),
            page.bottomLeft(),
        };

        painter.drawPolygon(outline, 5);
        painter.drawPolyline(&outline[1], 2);
    }
};

} // namespace

static QLabel* make_label(const QString& text, const QFont& font, uint32_t color, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFont(font);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignHCenter);

    label->setFixedWidth(TEXT_WIDTH);
    label->setFixedHeight(label->heightForWidth(TEXT_WIDTH));
    label->setStyleSheet(QStringLiteral("color: %1").arg(token(color).name()));
    return label;
}

binary_placeholder::binary_placeholder(QWidget* parent, const QString& name, const QByteArray& utf16_encoding)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(SECTION_GAP);
    layout->addStretch(1);
    layout->addWidget(new document_glyph(this), 0, Qt::AlignHCenter);
    layout->addWidget(make_label(QStringLiteral("%1 looks binary").arg(name), ui_font(TITLE_SIZE, QFont::DemiBold),
                                 STLXTHEME_TEXT, this), 0, Qt::AlignHCenter);
    layout->addWidget(make_label(QStringLiteral("It has NUL bytes in its first 64 KB. As text it shows them as marks, "
                                                "and saving keeps every byte."),
                                 ui_font(STLXTHEME_BODY_SIZE), STLXTHEME_TEXT_SECONDARY, this), 0, Qt::AlignHCenter);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(BUTTON_GAP);
    buttons->addStretch(1);
    QPushButton* first_choice = nullptr;
    if (!utf16_encoding.isEmpty()) {
        auto* utf16 = new QPushButton(QStringLiteral("Open as %1").arg(encoding_label(utf16_encoding)), this);
        connect(utf16, &QPushButton::clicked, this, [this, utf16_encoding] { on_open_as(utf16_encoding); });
        buttons->addWidget(utf16);
        first_choice = utf16;
    }

    auto* text = new QPushButton(QStringLiteral("Open as Text"), this);
    connect(text, &QPushButton::clicked, this, [this] { on_open_as(LATIN_1.toByteArray()); });
    buttons->addWidget(text);

    auto* close = new QPushButton(QStringLiteral("Close Tab"), this);
    connect(close, &QPushButton::clicked, this, [this] { on_close(); });
    buttons->addWidget(close);
    buttons->addStretch(1);

    layout->addLayout(buttons);
    layout->addStretch(2);
    setFocusProxy(first_choice ? first_choice : text);
}

// The placeholder stands where the text would, on the same opaque pocket
void binary_placeholder::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_OPAQUE_POCKET));
}
