#ifndef SHEEN_CHROME_H
#define SHEEN_CHROME_H

/* Fonts and colors of Sheen's own surfaces, the tab strip, banners, sheets and status bar, from the theme's tokens.
 * Sizes are em pixels, set as points at the 96 dpi Stellux screens report, since a QFont takes whole pixels only. */
#include <QColor>
#include <QFont>

#include <stlxtheme/theme.h>

#include <cstdint>

inline constexpr qreal POINTS_PER_PIXEL = 72.0 / 96.0;

/* The tone of a banner glyph that is no failure and of a file still arriving, cream so that amber keeps to the
 * user's own pending changes */
inline constexpr uint32_t NOTICE_TONE = STLXTHEME_TEXT;

inline QColor token(uint32_t argb) {
    return QColor::fromRgba(argb);
}

inline QFont ui_font(float em_px, QFont::Weight weight = QFont::Normal) {
    QFont font(QString::fromLatin1(STLXTHEME_UI_FONT_FAMILY));
    font.setPointSizeF(em_px * POINTS_PER_PIXEL);
    font.setWeight(weight);
    return font;
}

inline QFont mono_font(float em_px) {
    QFont font(QString::fromLatin1(STLXTHEME_MONO_FONT_FAMILY));
    font.setPointSizeF(em_px * POINTS_PER_PIXEL);
    return font;
}

#endif
