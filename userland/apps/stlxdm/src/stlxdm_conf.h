#ifndef STLXDM_CONF_H
#define STLXDM_CONF_H

#include <stdint.h>

#define STLXDM_CONF_MAX_TASKBAR   16
#define STLXDM_CONF_MAX_SHORTCUTS 16
#define STLXDM_CONF_PATH          "/initrd/etc/stlxdm/stlxdm.conf"

typedef struct {
    char label[64];
    char path[256];
    char icon[256];
} stlxdm_conf_taskbar_entry_t;

typedef struct {
    char name[64];
    char key[64];
    char action[32];
    char path[256];
} stlxdm_conf_shortcut_t;

typedef struct {
    /* [desktop] */
    char     wallpaper[256];
    uint32_t bg_color;
    int      has_bg_color;

    /* [theme] */
    uint32_t bar_color;
    int      has_bar_color;
    uint32_t bar_font_size;
    int      has_bar_font_size;
    uint32_t accent_color;
    int      has_accent_color;

    /* [taskbar:*] */
    stlxdm_conf_taskbar_entry_t taskbar[STLXDM_CONF_MAX_TASKBAR];
    int                         taskbar_count;

    /* [shortcut:*] */
    stlxdm_conf_shortcut_t shortcuts[STLXDM_CONF_MAX_SHORTCUTS];
    int                    shortcut_count;
} stlxdm_conf_t;

int  stlxdm_conf_load(stlxdm_conf_t* conf, const char* path);
void stlxdm_conf_init(stlxdm_conf_t* conf);

#endif /* STLXDM_CONF_H */
