#ifndef STLXDM_CONFIG_HPP
#define STLXDM_CONFIG_HPP

#include <cstdint>

constexpr const char* DM_CONF_PATH = "/etc/stlxdm/stlxdm.conf";
constexpr const char* DM_CONF_DEFAULT_ICON =
    "/etc/res/icons/icon_unknown_32x32.bmp";

constexpr uint32_t DM_CONF_MAX_TASKBAR = 16;
constexpr uint32_t DM_CONF_MAX_SHORTCUTS = 8;
constexpr uint32_t DM_CONF_MAX_AUTOSTART = 8;

/* One pinned launcher in the dock, order preserved from the file */
struct dm_conf_pin {
    char label[64];
    char path[256];
    char icon_path[256];
};

/* One global key binding. The key string is a plus-separated chord
 * such as ctrl+alt+t, and exec is the only action. */
struct dm_conf_shortcut {
    char key[64];
    char action[32];
    char path[256];
};

/* One program spawned at startup, args space separated */
struct dm_conf_autostart {
    char path[256];
    char args[256];
};

/**
 * The desktop configuration, parsed from an INI-style file. Every
 * field holds its default until the file overrides it, so a missing
 * or partial file always yields a usable desktop.
 */
struct dm_config {
    /* [desktop] wallpaper scales to cover the screen, an empty path
     * falls back to the flat bg_color */
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

    dm_conf_pin pins[DM_CONF_MAX_TASKBAR];
    uint32_t pin_count;

    dm_conf_shortcut shortcuts[DM_CONF_MAX_SHORTCUTS];
    uint32_t shortcut_count;

    dm_conf_autostart autostart[DM_CONF_MAX_AUTOSTART];
    uint32_t autostart_count;
};

/**
 * @brief Resets every field to the built-in defaults.
 */
void dm_config_defaults(dm_config& conf);

/**
 * @brief Loads the configuration file over the defaults.
 * @return 0 on success, -1 when the file cannot be read (the config
 *         holds the defaults either way).
 */
int dm_config_load(dm_config& conf, const char* path);

#endif
