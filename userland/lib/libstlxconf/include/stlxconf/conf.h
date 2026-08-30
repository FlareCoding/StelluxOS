#ifndef STLXCONF_CONF_H
#define STLXCONF_CONF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STLXCONF_PATH          "/etc/stlxdm/stlxdm.conf"
#define STLXCONF_DEFAULT_ICON  "/etc/res/icons/icon_unknown_32x32.bmp"
#define STLXCONF_MAX_TASKBAR   16
#define STLXCONF_MAX_SHORTCUTS 8
#define STLXCONF_MAX_AUTOSTART 8

/* One pinned launcher in the dock, order preserved from the file.
 * The name is the section suffix, kept for faithful re-serialization. */
typedef struct {
    char name[64];
    char label[64];
    char path[256];
    char icon_path[256];
} stlxconf_pin_t;

/* One global key binding. The key string is a plus-separated chord
 * such as ctrl+alt+t, and exec is the only action. */
typedef struct {
    char name[64];
    char key[64];
    char action[32];
    char path[256];
} stlxconf_shortcut_t;

/* One program spawned at startup, args space separated */
typedef struct {
    char name[64];
    char path[256];
    char args[256];
} stlxconf_autostart_t;

/**
 * The desktop configuration, parsed from an INI-style file. Every
 * field holds its default until the file overrides it, so a missing
 * or partial file always yields a usable desktop.
 */
typedef struct {
    /* [desktop] wallpaper is scaled to cover the screen, an empty
     * path falls back to the flat bg_color */
    uint32_t bg_color;
    char     wallpaper[256];

    /* [theme] bar colors and the bar text size */
    uint32_t bar_color;
    uint32_t bar_font_size;
    uint32_t accent_color;
    uint32_t text_color;

    /* [taskbar] dock geometry */
    uint32_t taskbar_height;
    uint32_t taskbar_icon_size;
    uint32_t taskbar_spacing;

    /* [input] keyboard repeat, a delay of 0 disables it */
    uint32_t key_repeat_delay_ms;
    uint32_t key_repeat_interval_ms;

    stlxconf_pin_t pins[STLXCONF_MAX_TASKBAR];
    uint32_t pin_count;

    stlxconf_shortcut_t shortcuts[STLXCONF_MAX_SHORTCUTS];
    uint32_t shortcut_count;

    stlxconf_autostart_t autostart[STLXCONF_MAX_AUTOSTART];
    uint32_t autostart_count;
} stlxconf_t;

/**
 * @brief Resets every field to the built-in defaults.
 */
void stlxconf_defaults(stlxconf_t* conf);

/**
 * @brief Loads the configuration file over the defaults.
 * @return 0 on success, -1 when the file cannot be read (the config
 *         holds the defaults either way).
 */
int stlxconf_load(stlxconf_t* conf, const char* path);

/**
 * @brief Writes the configuration back as canonical INI text.
 *
 * Sections come out grouped in a fixed order, so a load of the
 * written file always reproduces the same configuration.
 * @return 0 on success, -1 when the file cannot be written.
 */
int stlxconf_save(const stlxconf_t* conf, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* STLXCONF_CONF_H */
