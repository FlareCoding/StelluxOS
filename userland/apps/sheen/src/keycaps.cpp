#include "keycaps.h"
#include "chrome.h"

#include <QFontMetrics>
#include <QPainter>

struct key_name {
    const char* qt;
    const char* shown;
};

static constexpr int KEYCAP_HEIGHT = 18;
static constexpr int KEYCAP_PADDING_X = 5;
static constexpr int KEYCAP_GAP = 3;
static constexpr int HINT_HEIGHT = 20;
static constexpr int HINT_TEXT_GAP = 6;
static constexpr qreal HALF_PIXEL = 0.5;

// Keys the keyboard map names differently from Qt
static constexpr key_name KEY_NAMES[] = {
    { "Return", "Enter" },
    { "PgDown", "PgDn" },
};

QStringList shortcut_keys(const QString& shortcut) {
    if (shortcut.isEmpty()) {
        return QStringList();
    }

    QStringList keys = shortcut.split(QLatin1Char('+'));
    if (shortcut.endsWith(QLatin1String("++"))) {
        keys.removeLast();
        keys.last() = QStringLiteral("+");
    }

    for (QString& key : keys) {
        for (const key_name& name : KEY_NAMES) {
            if (key == QLatin1String(name.qt)) {
                key = QString::fromLatin1(name.shown);
            }
        }
    }

    return keys;
}

int keycaps_width(const QStringList& keys) {
    QFontMetrics metrics(mono_font(STLXTHEME_CAPTION_SIZE));
    int width = 0;
    for (const QString& key : keys) {
        width += metrics.horizontalAdvance(key) + 2 * KEYCAP_PADDING_X + KEYCAP_GAP;
    }

    return keys.isEmpty() ? 0 : width - KEYCAP_GAP;
}

void paint_keycaps(QPainter* painter, const QStringList& keys, int left, int center_y, const QColor& color) {
    QFont font = mono_font(STLXTHEME_CAPTION_SIZE);
    QFontMetrics metrics(font);
    painter->setFont(font);

    int x = left;
    for (const QString& key : keys) {
        int width = metrics.horizontalAdvance(key) + 2 * KEYCAP_PADDING_X;
        QRectF cap(x + HALF_PIXEL, center_y - KEYCAP_HEIGHT / 2 + HALF_PIXEL, width - 1, KEYCAP_HEIGHT - 1);
        painter->setPen(QPen(token(STLXTHEME_LINE), 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(cap, STLXTHEME_CHIP_RADIUS, STLXTHEME_CHIP_RADIUS);

        painter->setPen(color);
        painter->drawText(cap, Qt::AlignCenter, key);
        x += width + KEYCAP_GAP;
    }
}

keycap_hint::keycap_hint(const QStringList& keys, const QString& text, QWidget* parent)
    : QWidget(parent), m_keys(keys), m_text(text) {
    QFontMetrics metrics(ui_font(STLXTHEME_CAPTION_SIZE));
    setFixedSize(keycaps_width(keys) + HINT_TEXT_GAP + metrics.horizontalAdvance(text), HINT_HEIGHT);
}

void keycap_hint::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paint_keycaps(&painter, m_keys, 0, height() / 2, token(STLXTHEME_TEXT_DIM));

    QRect room = rect().adjusted(keycaps_width(m_keys) + HINT_TEXT_GAP, 0, 0, 0);
    painter.setFont(ui_font(STLXTHEME_CAPTION_SIZE));
    painter.setPen(token(STLXTHEME_TEXT_DIM));
    painter.drawText(room, Qt::AlignLeft | Qt::AlignVCenter, m_text);
}
