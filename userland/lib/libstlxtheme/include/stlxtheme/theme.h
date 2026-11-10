#ifndef STLXTHEME_THEME_H
#define STLXTHEME_THEME_H

/* The desktop's palette, control metrics and typefaces. Colors are 0xAARRGGBB
 * with straight alpha, and lengths are pixels. */

/* Palette */
#define STLXTHEME_WINDOW_BG     0xFF1E1E2E
#define STLXTHEME_ALTERNATE_ROW 0xFF212131 /* a step off the window, under the hover and selection fills */
#define STLXTHEME_SURFACE       0xFF313244
#define STLXTHEME_SURFACE_HOVER 0xFF45475A
#define STLXTHEME_SURFACE_PRESS 0xFF585B70
#define STLXTHEME_RAISED_DARK   0xFF181825
#define STLXTHEME_SHADOW        0xFF11111B
#define STLXTHEME_ACCENT        0xFF89B4FA
#define STLXTHEME_ACCENT_HOVER  0xFFA5C8FC
#define STLXTHEME_ACCENT_PRESS  0xFF6E9DE8
#define STLXTHEME_DANGER        0xFFF38BA8
#define STLXTHEME_TEXT          0xFFCDD6F4
#define STLXTHEME_TEXT_DIM      0xFF585B70

/* Control metrics */
#define STLXTHEME_CONTROL_HEIGHT          28
#define STLXTHEME_CONTROL_RADIUS          6
#define STLXTHEME_BUTTON_PADDING_X        12
#define STLXTHEME_FIELD_PADDING_X         8
#define STLXTHEME_CHECK_SIZE              16
#define STLXTHEME_CHECK_RADIUS            4
#define STLXTHEME_CHECK_LABEL_SPACING     8
#define STLXTHEME_MENU_PADDING            4
#define STLXTHEME_MENU_ROW_HEIGHT         24
#define STLXTHEME_MENU_ROW_PADDING_X      12
#define STLXTHEME_MENU_SEPARATOR_HEIGHT   5
#define STLXTHEME_SCROLL_THUMB_WIDTH      4
#define STLXTHEME_SCROLL_THUMB_RADIUS     2
#define STLXTHEME_SCROLL_THUMB_MIN_LENGTH 16

/* Typefaces */
#define STLXTHEME_MONO_FONT_PATH        "/etc/res/fonts/UbuntuMono-Regular.ttf"
#define STLXTHEME_UI_FONT_FAMILY        "Inter"
#define STLXTHEME_UI_FONT_PATH          "/etc/res/fonts/Inter-Regular.ttf"
#define STLXTHEME_UI_FONT_MEDIUM_PATH   "/etc/res/fonts/Inter-Medium.ttf"
#define STLXTHEME_UI_FONT_SEMIBOLD_PATH "/etc/res/fonts/Inter-SemiBold.ttf"

#endif /* STLXTHEME_THEME_H */
