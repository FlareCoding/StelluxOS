#ifndef STLXDM_CONF_H
#define STLXDM_CONF_H

#include <stdint.h>

#define STLXDM_CONF_PATH              "/initrd/etc/stlxdm/stlxdm.conf"
#define STLXDM_CONF_DEFAULT_ICON_PATH "/initrd/res/icons/icon_unknown_32x32.bmp"
#define STLXDM_CONF_MAX_TASKBAR_ITEMS 16
#define STLXDM_CONF_MAX_SHORTCUTS     8
#define STLXDM_CONF_MAX_AUTOSTART     8

typedef struct {
    char     label[64];
    char     path[256];
    char     icon_path[256];
} stlxdm_conf_taskbar_item_t;

typedef struct {
    char     name[64];
    char     key[64];
    char     action[32];
    char     path[256];
} stlxdm_conf_shortcut_t;

typedef struct {
    char     path[256];
} stlxdm_conf_autostart_t;

typedef struct {
    /* [desktop] */
    uint32_t bg_color;

    /* [theme] */
    uint32_t bar_color;
    uint32_t bar_font_size;
    uint32_t accent_color;
    uint32_t text_color;

    /* [taskbar] global settings */
    uint32_t taskbar_height;
    uint32_t taskbar_icon_size;
    uint32_t taskbar_spacing;

    /* [taskbar:*] items (order preserved from file) */
    stlxdm_conf_taskbar_item_t taskbar_items[STLXDM_CONF_MAX_TASKBAR_ITEMS];
    int taskbar_item_count;

    /* [shortcut:*] */
    stlxdm_conf_shortcut_t shortcuts[STLXDM_CONF_MAX_SHORTCUTS];
    int shortcut_count;

    /* [autostart:*] */
    stlxdm_conf_autostart_t autostart[STLXDM_CONF_MAX_AUTOSTART];
    int autostart_count;
} stlxdm_config_t;

void stlxdm_conf_defaults(stlxdm_config_t* conf);
int  stlxdm_conf_load(stlxdm_config_t* conf, const char* path);

#endif /* STLXDM_CONF_H */
