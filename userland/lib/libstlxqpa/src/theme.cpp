#include "theme.h"
#include "integration.h"
#include "style.h"

#include <QFileInfo>
#include <QStringList>
#include <QVariant>

#include <stlxtheme/theme.h>

static const QColor RECESS = QColor::fromRgba(STLXTHEME_RECESS);
static const QColor CARD = QColor::fromRgba(STLXTHEME_CARD);
static const QColor SURFACE = QColor::fromRgba(STLXTHEME_SURFACE);
static const QColor SURFACE_HOVER = QColor::fromRgba(STLXTHEME_SURFACE_HOVER);
static const QColor SURFACE_PRESS = QColor::fromRgba(STLXTHEME_SURFACE_PRESS);
static const QColor SURFACE_SELECTED = QColor::fromRgba(STLXTHEME_SURFACE_SELECTED);
static const QColor LINE_SUBTLE = QColor::fromRgba(STLXTHEME_LINE_SUBTLE);
static const QColor LINE = QColor::fromRgba(STLXTHEME_LINE);
static const QColor SHADE = QColor::fromRgba(STLXTHEME_SHADE);
static const QColor INK = QColor::fromRgba(STLXTHEME_INK);
static const QColor MENU_BG = QColor::fromRgba(STLXTHEME_MENU_BG);
static const QColor ACCENT = QColor::fromRgba(STLXTHEME_ACCENT);
static const QColor ACCENT_PRESS = QColor::fromRgba(STLXTHEME_ACCENT_PRESS);
static const QColor DANGER = QColor::fromRgba(STLXTHEME_DANGER);
static const QColor TEXT = QColor::fromRgba(STLXTHEME_TEXT);
static const QColor TEXT_DIM = QColor::fromRgba(STLXTHEME_TEXT_DIM);
static const QColor TEXT_DISABLED = QColor::fromRgba(STLXTHEME_TEXT_DISABLED);

static const qreal POINTS_PER_INCH = 72;

static const char* FOLDER_ICON = "/etc/res/icons/folder-32.png";
static const char* PROGRAM_ICON = "/etc/res/icons/doc-exec-32.png";
static const char* IMAGE_ICON = "/etc/res/icons/doc-image-32.png";
static const char* FILE_ICON = "/etc/res/icons/doc-text-32.png";

static const char* IMAGE_MIME_PREFIX = "image/";

static const QFile::Permissions EXECUTE_PERMISSIONS = QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;

static QPalette desktop_palette() {
    QPalette palette;

    // Transparent so every Qt window shows stlxdm's glass through its background
    palette.setColor(QPalette::Window, Qt::transparent);

    palette.setColor(QPalette::WindowText, TEXT);
    palette.setColor(QPalette::Base, RECESS);
    palette.setColor(QPalette::AlternateBase, CARD);
    palette.setColor(QPalette::Text, TEXT);
    palette.setColor(QPalette::PlaceholderText, TEXT_DIM);

    // Control faces at rest, hovered and pressed, the resting edge, the darker panel fill and the shadow color
    palette.setColor(QPalette::Button, SURFACE);
    palette.setColor(QPalette::Midlight, SURFACE_HOVER);
    palette.setColor(QPalette::Light, SURFACE_PRESS);
    palette.setColor(QPalette::Mid, LINE);
    palette.setColor(QPalette::Dark, SHADE);
    palette.setColor(QPalette::Shadow, INK);

    // Selection is a cream overlay, and the accent marks a focused field, the default button and links
    palette.setColor(QPalette::Highlight, SURFACE_SELECTED);
    palette.setColor(QPalette::HighlightedText, TEXT);
    palette.setColor(QPalette::Accent, ACCENT);
    palette.setColor(QPalette::Link, ACCENT);
    palette.setColor(QPalette::LinkVisited, ACCENT_PRESS);

    palette.setColor(QPalette::ButtonText, TEXT);
    palette.setColor(QPalette::BrightText, DANGER);
    palette.setColor(QPalette::ToolTipBase, MENU_BG);
    palette.setColor(QPalette::ToolTipText, TEXT);

    palette.setColor(QPalette::Disabled, QPalette::WindowText, TEXT_DISABLED);
    palette.setColor(QPalette::Disabled, QPalette::Text, TEXT_DISABLED);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, TEXT_DISABLED);

    palette.setColor(QPalette::Disabled, QPalette::Base, Qt::transparent);
    palette.setColor(QPalette::Disabled, QPalette::Button, Qt::transparent);
    palette.setColor(QPalette::Disabled, QPalette::Mid, LINE_SUBTLE);

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
