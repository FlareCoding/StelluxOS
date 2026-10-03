#ifndef STLXQPA_THEME_H
#define STLXQPA_THEME_H

#include <QFont>
#include <QPalette>
#include <qpa/qplatformtheme.h>

// The desktop's look for Qt programs: its palette, its fonts and the stellux style
class QStelluxTheme : public QPlatformTheme {
public:
    static constexpr const char* NAME = "stellux";

    QStelluxTheme();

    const QPalette* palette(Palette type) const override;
    const QFont* font(Font type) const override;
    QVariant themeHint(ThemeHint hint) const override;
    Qt::ColorScheme colorScheme() const override;

private:
    QPalette m_palette;
    QFont m_font;
    QFont m_small_font;
    QFont m_button_font;
};

#endif
