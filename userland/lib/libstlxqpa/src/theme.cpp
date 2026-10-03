#include "theme.h"
#include "style.h"

#include <QFileInfo>
#include <QStringList>
#include <QVariant>

// The desktop's palette, the one stlxui draws its widgets with
static const QColor WINDOW_BG(0x1E1E2E);
static const QColor ALTERNATE_ROW(0x212131);  // a step off the window, under the hover and selection fills
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

static const char* FOLDER_ICON = "/etc/res/icons/icon_fm_folder_16x16.bmp";
static const char* PROGRAM_ICON = "/etc/res/icons/icon_fm_exec_16x16.bmp";
static const char* IMAGE_ICON = "/etc/res/icons/icon_fm_image_16x16.bmp";
static const char* TEXT_ICON = "/etc/res/icons/icon_fm_text_16x16.bmp";
static const char* FILE_ICON = "/etc/res/icons/icon_fm_file_16x16.bmp";

static const char* IMAGE_MIME_PREFIX = "image/";
static const char* TEXT_MIME_TYPE = "text/plain";

static const QFile::Permissions EXECUTE_PERMISSIONS = QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;

static QPalette desktop_palette() {
    QPalette palette;
    palette.setColor(QPalette::Window, WINDOW_BG);
    palette.setColor(QPalette::WindowText, TEXT);
    palette.setColor(QPalette::Base, SURFACE);
    palette.setColor(QPalette::AlternateBase, ALTERNATE_ROW);
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
    , m_button_font(desktop_font(FONT_PIXEL_SIZE, QFont::Medium))
    , m_folder_icon(QString::fromLatin1(FOLDER_ICON))
    , m_program_icon(QString::fromLatin1(PROGRAM_ICON))
    , m_image_icon(QString::fromLatin1(IMAGE_ICON))
    , m_text_icon(QString::fromLatin1(TEXT_ICON))
    , m_file_icon(QString::fromLatin1(FILE_ICON)) {
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

// Qt calls this from its file system threads as well, so it only reads what the constructor loaded
QIcon QStelluxTheme::fileIcon(const QFileInfo& info, IconOptions) const {
    if (info.isDir()) {
        return m_folder_icon;
    }

    // A program is a file carrying an execute permission bit
    if (info.isFile() && info.permissions().testAnyFlags(EXECUTE_PERMISSIONS)) {
        return m_program_icon;
    }

    // By name alone, since reading each file's contents would slow every listing
    QMimeType type = m_mime_database.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
    if (type.name().startsWith(QLatin1String(IMAGE_MIME_PREFIX))) {
        return m_image_icon;
    }

    if (type.inherits(QLatin1String(TEXT_MIME_TYPE))) {
        return m_text_icon;
    }

    return m_file_icon;
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
