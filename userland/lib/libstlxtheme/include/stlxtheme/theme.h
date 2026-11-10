#ifndef STLXTHEME_THEME_H
#define STLXTHEME_THEME_H

/* The desktop's palette, control metrics and typefaces. Colors are 0xAARRGGBB
 * with straight alpha, and lengths are pixels. */

/* Text */
#define STLXTHEME_TEXT           0xFFF6EEE1 /* titles, values and body text */
#define STLXTHEME_TEXT_SECONDARY 0xFFC4BAAB /* quieter text and marks, such as menu items at rest and a checked box */
#define STLXTHEME_TEXT_DIM       0xFFA39A8D /* the dimmest readable text, such as captions and placeholders */
#define STLXTHEME_TEXT_DISABLED  0xFF6E675D
#define STLXTHEME_TERMINAL_TEXT  0xFFE8E0D2

/* The one accent, and the danger color */
#define STLXTHEME_ACCENT       0xFFFFB224
#define STLXTHEME_ACCENT_HOVER 0xFFFFBA47
#define STLXTHEME_ACCENT_PRESS 0xFFD1911B
#define STLXTHEME_ACCENT_TEXT  0xFF1E1206
#define STLXTHEME_DANGER       0xFFB4483E

/* Lines, all brass */
#define STLXTHEME_LINE_SUBTLE 0x1FE2C496 /* the faintest lines, such as a disabled control's border */
#define STLXTHEME_LINE        0x36E2C496 /* control borders */
#define STLXTHEME_LINE_STRONG 0x52E2C496 /* hover and focus borders and scroll thumbs */

/* Overlays laid over a window's background, and the menu panel */
#define STLXTHEME_SURFACE          0x0DFFF0D7 /* button faces */
#define STLXTHEME_SURFACE_HOVER    0x17FFF0D7
#define STLXTHEME_SURFACE_PRESS    0x4D000000
#define STLXTHEME_ROW_HOVER        0x0FFFF0D7
#define STLXTHEME_SURFACE_SELECTED 0x17FFF0D7 /* highlighted items, such as a selected row and the hovered menu item */
#define STLXTHEME_SELECTED_RING    0x33E2C496
#define STLXTHEME_MENU_BG          0xF5181511

/* Dark overlays and solid fills */
#define STLXTHEME_WINDOW_BG 0xFF12100D /* an opaque window's background, also the default desktop color */
#define STLXTHEME_CARD      0x24080502 /* fills a step darker than the window background, such as alternate rows */
#define STLXTHEME_SHADE     0x42080502 /* darker panels, such as sidebars */
#define STLXTHEME_RECESS    0x80080502 /* the recessed parts of controls, such as fields and unchecked check boxes */
#define STLXTHEME_INK       0xFF060402 /* the shadow color */

/* Corner radii */
#define STLXTHEME_CONTROL_RADIUS   6
#define STLXTHEME_MENU_ITEM_RADIUS 4

/* Sizes */
#define STLXTHEME_CONTROL_HEIGHT         32
#define STLXTHEME_LIST_ROW_HEIGHT        30
#define STLXTHEME_FOCUS_RULE_WIDTH       2
#define STLXTHEME_TERMINAL_LINE_HEIGHT   22

/* Control metrics */
#define STLXTHEME_BUTTON_PADDING_X        14
#define STLXTHEME_ACCENT_BUTTON_PADDING_X 18
#define STLXTHEME_FIELD_PADDING_X         10
#define STLXTHEME_CHECK_SIZE              16
#define STLXTHEME_CHECK_RADIUS            4
#define STLXTHEME_CHECK_LABEL_SPACING     8
#define STLXTHEME_MENU_PADDING            4
#define STLXTHEME_MENU_ROW_HEIGHT         28
#define STLXTHEME_MENU_ROW_PADDING_X      8
#define STLXTHEME_MENU_SEPARATOR_HEIGHT   5
#define STLXTHEME_SCROLL_THUMB_WIDTH      4
#define STLXTHEME_SCROLL_THUMB_RADIUS     2
#define STLXTHEME_SCROLL_THUMB_MIN_LENGTH 16

/* Typefaces */
#define STLXTHEME_UI_FONT_FAMILY        "IBM Plex Sans"
#define STLXTHEME_UI_FONT_PATH          "/etc/res/fonts/IBMPlexSans-Regular.ttf"
#define STLXTHEME_UI_FONT_MEDIUM_PATH   "/etc/res/fonts/IBMPlexSans-Medium.ttf"
#define STLXTHEME_UI_FONT_SEMIBOLD_PATH "/etc/res/fonts/IBMPlexSans-SemiBold.ttf"
#define STLXTHEME_MONO_FONT_FAMILY      "IBM Plex Mono"
#define STLXTHEME_MONO_FONT_PATH        "/etc/res/fonts/IBMPlexMono-Regular.ttf"

/* Type sizes in em pixels, chosen so Plex's x-height lands on whole pixels */
#define STLXTHEME_BODY_SIZE       13.5f
#define STLXTHEME_CAPTION_SIZE    11.5f
#define STLXTHEME_TERMINAL_SIZE   15.5f

#endif /* STLXTHEME_THEME_H */
