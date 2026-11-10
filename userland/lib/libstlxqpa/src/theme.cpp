#include "theme.h"
#include "integration.h"
#include "style.h"

#include <QFileInfo>
#include <QStringList>
#include <QVariant>

#include <stlxtheme/theme.h>

static const QColor WINDOW_BG = QColor::fromRgba(STLXTHEME_WINDOW_BG);
static const QColor CARD = QColor::fromRgba(STLXTHEME_CARD);
static const QColor SURFACE = QColor::fromRgba(STLXTHEME_SURFACE);
static const QColor SURFACE_HOVER = QColor::fromRgba(STLXTHEME_SURFACE_HOVER);
static const QColor SURFACE_PRESS = QColor::fromRgba(STLXTHEME_SURFACE_PRESS);
static const QColor SHADE = QColor::fromRgba(STLXTHEME_SHADE);
static const QColor INK = QColor::fromRgba(STLXTHEME_INK);
static const QColor ACCENT = QColor::fromRgba(STLXTHEME_ACCENT);
static const QColor ACCENT_PRESS = QColor::fromRgba(STLXTHEME_ACCENT_PRESS);
static const QColor DANGER = QColor::fromRgba(STLXTHEME_DANGER);
static const QColor TEXT = QColor::fromRgba(STLXTHEME_TEXT);
static const QColor TEXT_DIM = QColor::fromRgba(STLXTHEME_TEXT_DIM);

static const qreal POINTS_PER_INCH = 72;

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
    palette.setColor(QPalette::AlternateBase, CARD);
    palette.setColor(QPalette::Text, TEXT);

    palette.setColor(QPalette::PlaceholderText, TEXT_DIM);
    palette.setColor(QPalette::Button, SURFACE);
    palette.setColor(QPalette::ButtonText, TEXT);
    palette.setColor(QPalette::BrightText, DANGER);

    palette.setColor(QPalette::Light, SURFACE_PRESS);
    palette.setColor(QPalette::Midlight, SURFACE_HOVER);
    palette.setColor(QPalette::Mid, SURFACE);
    palette.setColor(QPalette::Dark, SHADE);
    palette.setColor(QPalette::Shadow, INK);

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

// Built from the family, since a default QFont reads the application font before this theme exists. QFont takes
// only whole pixel sizes, so a half pixel em size is set as points at the screens' dots per inch.
static QFont desktop_font(const char* family, float em_px, QFont::Weight weight) {
    QFont font(QString::fromLatin1(family));
    font.setPointSizeF(em_px * POINTS_PER_INCH / SCREEN_LOGICAL_DPI);
    font.setWeight(weight);
    return font;
}

// Button labels take the medium weight, as stlxui sets them
QStelluxTheme::QStelluxTheme()
    : m_palette(desktop_palette())
    , m_font(desktop_font(STLXTHEME_UI_FONT_FAMILY, STLXTHEME_BODY_SIZE, QFont::Normal))
    , m_small_font(desktop_font(STLXTHEME_UI_FONT_FAMILY, STLXTHEME_CAPTION_SIZE, QFont::Normal))
    , m_button_font(desktop_font(STLXTHEME_UI_FONT_FAMILY, STLXTHEME_BODY_SIZE, QFont::Medium))
    , m_fixed_font(desktop_font(STLXTHEME_MONO_FONT_FAMILY, STLXTHEME_BODY_SIZE, QFont::Normal))
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
        case FixedFont:
            return &m_fixed_font;
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
