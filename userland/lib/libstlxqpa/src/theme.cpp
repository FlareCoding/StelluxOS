#include "theme.h"
#include "style.h"

#include <QStringList>
#include <QVariant>

// The desktop's palette, the one stlxui draws its widgets with
static const QColor WINDOW_BG(0x1E1E2E);
static const QColor SURFACE(0x313244);
static const QColor SURFACE_HOVER(0x45475A);
static const QColor SURFACE_PRESS(0x585B70);
static const QColor RAISED_DARK(0x181825);
static const QColor SHADOW(0x11111B);
static const QColor ACCENT(0x89B4FA);
static const QColor ACCENT_PRESS(0x6E9DE8);
static const QColor DANGER(0xF38BA8);
static const QColor TEXT(0xCDD6F4);
static const QColor TEXT_DIM(0x585B70);

// stlxgfx sizes a face from ascender to descender, which makes the desktop's
// 14 px and 12 px text Inter faces with 12 px and 10 px ems, the sizes Qt takes
static const char* FONT_FAMILY = "Inter";
static const int FONT_PIXEL_SIZE = 12;
static const int SMALL_FONT_PIXEL_SIZE = 10;

static QPalette desktop_palette() {
    QPalette palette;
    palette.setColor(QPalette::Window, WINDOW_BG);
    palette.setColor(QPalette::WindowText, TEXT);
    palette.setColor(QPalette::Base, SURFACE);
    palette.setColor(QPalette::AlternateBase, SURFACE_HOVER);
    palette.setColor(QPalette::Text, TEXT);
    palette.setColor(QPalette::PlaceholderText, TEXT_DIM);
    palette.setColor(QPalette::Button, SURFACE);
    palette.setColor(QPalette::ButtonText, TEXT);
    palette.setColor(QPalette::BrightText, DANGER);
    palette.setColor(QPalette::Light, SURFACE_PRESS);
    palette.setColor(QPalette::Midlight, SURFACE_HOVER);
    palette.setColor(QPalette::Mid, SURFACE);
    palette.setColor(QPalette::Dark, RAISED_DARK);
    palette.setColor(QPalette::Shadow, SHADOW);
    palette.setColor(QPalette::Highlight, ACCENT);
    palette.setColor(QPalette::HighlightedText, WINDOW_BG);
    palette.setColor(QPalette::Accent, ACCENT);
    palette.setColor(QPalette::Link, ACCENT);
    palette.setColor(QPalette::LinkVisited, ACCENT_PRESS);
    palette.setColor(QPalette::ToolTipBase, SURFACE);
    palette.setColor(QPalette::ToolTipText, TEXT);

    palette.setColor(QPalette::Disabled, QPalette::WindowText, TEXT_DIM);
    palette.setColor(QPalette::Disabled, QPalette::Text, TEXT_DIM);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, TEXT_DIM);
    palette.setColor(QPalette::Disabled, QPalette::Base, WINDOW_BG);
    palette.setColor(QPalette::Disabled, QPalette::Button, WINDOW_BG);

    return palette;
}

// Built from the family, since a default QFont reads the application font before this theme exists
static QFont desktop_font(int pixel_size, QFont::Weight weight) {
    QFont font(QString::fromLatin1(FONT_FAMILY));
    font.setPixelSize(pixel_size);
    font.setWeight(weight);
    return font;
}

// Button labels take the medium weight, as stlxui sets them
QStelluxTheme::QStelluxTheme()
    : m_palette(desktop_palette())
    , m_font(desktop_font(FONT_PIXEL_SIZE, QFont::Normal))
    , m_small_font(desktop_font(SMALL_FONT_PIXEL_SIZE, QFont::Normal))
    , m_button_font(desktop_font(FONT_PIXEL_SIZE, QFont::Medium)) {
}

const QPalette* QStelluxTheme::palette(Palette type) const {
    return type == SystemPalette ? &m_palette : nullptr;
}

const QFont* QStelluxTheme::font(Font type) const {
    switch (type) {
        case SystemFont:
            return &m_font;
        // Programs ask for it as QFontDatabase::SmallestReadableFont
        case MiniFont:
            return &m_small_font;
        case PushButtonFont:
        case ToolButtonFont:
            return &m_button_font;
        default:
            return nullptr;
    }
}

QVariant QStelluxTheme::themeHint(ThemeHint hint) const {
    if (hint == StyleNames) {
        return QStringList { QString::fromLatin1(QStelluxStyle::NAME) };
    }

    return QPlatformTheme::themeHint(hint);
}

Qt::ColorScheme QStelluxTheme::colorScheme() const {
    return Qt::ColorScheme::Dark;
}
