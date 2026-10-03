#include "theme.h"

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
// 14 px text an Inter face with a 12 px em, the size Qt takes
static const char* FONT_FAMILY = "Inter";
static const int FONT_PIXEL_SIZE = 12;
static const char* STYLE = "Fusion";

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

QStelluxTheme::QStelluxTheme()
    : m_palette(desktop_palette())
    , m_font(QString::fromLatin1(FONT_FAMILY)) {
    m_font.setPixelSize(FONT_PIXEL_SIZE);
}

const QPalette* QStelluxTheme::palette(Palette type) const {
    return type == SystemPalette ? &m_palette : nullptr;
}

const QFont* QStelluxTheme::font(Font type) const {
    return type == SystemFont ? &m_font : nullptr;
}

QVariant QStelluxTheme::themeHint(ThemeHint hint) const {
    if (hint == StyleNames) {
        return QStringList { QString::fromLatin1(STYLE) };
    }

    return QPlatformTheme::themeHint(hint);
}

Qt::ColorScheme QStelluxTheme::colorScheme() const {
    return Qt::ColorScheme::Dark;
}
