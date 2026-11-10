#ifndef STLXTHEME_THEME_H
#define STLXTHEME_THEME_H

/* The desktop's palette, control metrics and typefaces. Colors are 0xAARRGGBB
 * with straight alpha, and lengths are pixels. Glass is a surface that shows
 * the frosted wallpaper behind it through a tint, which the solid glass level
 * makes opaque. */

/* Glass colors: the smoke tints every glass surface is made of, its veil and its light */
#define STLXTHEME_GLASS_TINT_WARM    0xFF12100D
#define STLXTHEME_GLASS_TINT_NEUTRAL 0xFF0F0F11
#define STLXTHEME_GLASS_VEIL         0xFF404040 /* the grey veil over glass, most opaque at a surface's top edge */
#define STLXTHEME_GLASS_LIGHT        0xFFFFF0D7 /* the rim light and grain on glass */

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
#define STLXTHEME_DANGER_BEZEL 0x29000000 /* the ring of a window control whose face is `STLXTHEME_DANGER` */
#define STLXTHEME_DANGER_TEXT  0xFFFFF3E6

/* Lines, all brass */
#define STLXTHEME_LINE_SUBTLE 0x1FE2C496 /* the faintest lines, such as a disabled control's border */
#define STLXTHEME_LINE        0x36E2C496 /* control borders */
#define STLXTHEME_LINE_STRONG 0x52E2C496 /* hover and focus borders and scroll thumbs */
#define STLXTHEME_LINE_ON     0x3DE2C496 /* the ring of an open bar target */
#define STLXTHEME_RIM         0x4DE2C496 /* the edge of the focused window and of a menu */
#define STLXTHEME_RIM_DIM     0x26E2C496
#define STLXTHEME_BEZEL       0x42E2C496 /* the ring of a window control */
#define STLXTHEME_BEZEL_DIM   0x1CE2C496 /* on an unfocused window */

/* Unlit meter cells, brass */
#define STLXTHEME_METER_IDLE 0x38E2C496 /* the bar's load meter */

/* Overlays laid over a window's background, and the menu panel */
#define STLXTHEME_SURFACE          0x0DFFF0D7 /* button faces and the system menu's keycaps */
#define STLXTHEME_SURFACE_HOVER    0x17FFF0D7
#define STLXTHEME_SURFACE_PRESS    0x4D000000
#define STLXTHEME_ROW_HOVER        0x0FFFF0D7
#define STLXTHEME_SURFACE_SELECTED 0x17FFF0D7 /* highlighted items, such as a selected row and the hovered menu item */
#define STLXTHEME_SELECTED_RING    0x33E2C496
#define STLXTHEME_SURFACE_ON       0x1CFFF0D7 /* the face of an open bar target */
#define STLXTHEME_MENU_BG          0xF5181511

/* Dark overlays and solid fills */
#define STLXTHEME_WINDOW_BG 0xFF12100D /* an opaque window's background, also the default desktop color */
#define STLXTHEME_CARD      0x24080502 /* fills a step darker than the window background, such as alternate rows */
#define STLXTHEME_SHADE     0x42080502 /* darker panels, such as sidebars, and the terminal body */
#define STLXTHEME_RECESS    0x80080502 /* the recessed parts of controls, such as fields and unchecked check boxes */
#define STLXTHEME_INK       0xFF060402 /* the shadow color */

/* Window controls, the round minimize, maximize and close buttons of a title bar */
#define STLXTHEME_WINDOW_CONTROL           0x14FFF0D7 /* the face */
#define STLXTHEME_WINDOW_CONTROL_HOVER     0x33FFF0D7
#define STLXTHEME_WINDOW_CONTROL_PRESS     0x6B000000
#define STLXTHEME_WINDOW_CONTROL_DIM       0x09FFF0D7 /* the face on an unfocused window */
#define STLXTHEME_WINDOW_CONTROL_GLYPH     0xD1F6EEE1
#define STLXTHEME_WINDOW_CONTROL_GLYPH_DIM 0x4DF6EEE1

/* Corner radii */
#define STLXTHEME_WINDOW_RADIUS    10
#define STLXTHEME_CONTROL_RADIUS   6
#define STLXTHEME_MENU_RADIUS      8
#define STLXTHEME_MENU_ITEM_RADIUS 4
#define STLXTHEME_CHIP_RADIUS      4

/* Sizes */
#define STLXTHEME_TOP_BAR_HEIGHT         32
#define STLXTHEME_TITLE_BAR_HEIGHT       32
#define STLXTHEME_CONTROL_HEIGHT         32
#define STLXTHEME_LIST_ROW_HEIGHT        30
#define STLXTHEME_GLYPH_SIZE             16
#define STLXTHEME_WINDOW_CONTROL_SIZE    16
#define STLXTHEME_WINDOW_CONTROL_GAP     8
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
