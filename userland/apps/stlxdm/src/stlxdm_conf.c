#include "stlxdm_conf.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void stlxdm_conf_defaults(stlxdm_config_t* conf) {
    memset(conf, 0, sizeof(*conf));

    conf->bg_color        = 0xFF2D2D30;
    conf->bar_color       = 0xFF1E1E1E;
    conf->bar_font_size   = 14;
    conf->accent_color    = 0xFF888888;
    conf->text_color      = 0xFFCCCCCC;

    conf->taskbar_height    = 48;
    conf->taskbar_icon_size = 32;
    conf->taskbar_spacing   = 8;

    conf->taskbar_item_count = 0;
    conf->shortcut_count     = 0;
    conf->autostart_count    = 0;
}

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        end--;
    *end = '\0';
    return s;
}

static void safe_copy(char* dst, const char* src, size_t dst_size) {
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

typedef enum {
    SEC_NONE,
    SEC_DESKTOP,
    SEC_THEME,
    SEC_TASKBAR_GLOBAL,
    SEC_TASKBAR_ITEM,
    SEC_SHORTCUT,
    SEC_AUTOSTART
} section_type_t;

static void parse_line(stlxdm_config_t* conf, const char* line,
                       section_type_t* sec, int* item_idx) {
    char buf[512];
    safe_copy(buf, line, sizeof(buf));
    char* s = trim(buf);

    if (*s == '\0' || *s == '#' || *s == ';')
        return;

    if (*s == '[') {
        char* end = strchr(s, ']');
        if (!end) return;
        *end = '\0';
        char* section = s + 1;

        if (strcmp(section, "desktop") == 0) {
            *sec = SEC_DESKTOP;
        } else if (strcmp(section, "theme") == 0) {
            *sec = SEC_THEME;
        } else if (strcmp(section, "taskbar") == 0) {
            *sec = SEC_TASKBAR_GLOBAL;
        } else if (strncmp(section, "taskbar:", 8) == 0) {
            *sec = SEC_TASKBAR_ITEM;
            if (conf->taskbar_item_count < STLXDM_CONF_MAX_TASKBAR_ITEMS) {
                *item_idx = conf->taskbar_item_count++;
                stlxdm_conf_taskbar_item_t* it =
                    &conf->taskbar_items[*item_idx];
                memset(it, 0, sizeof(*it));
            } else {
                *item_idx = -1;
            }
        } else if (strncmp(section, "shortcut:", 9) == 0) {
            *sec = SEC_SHORTCUT;
            if (conf->shortcut_count < STLXDM_CONF_MAX_SHORTCUTS) {
                *item_idx = conf->shortcut_count++;
                stlxdm_conf_shortcut_t* sc = &conf->shortcuts[*item_idx];
                memset(sc, 0, sizeof(*sc));
                safe_copy(sc->name, section + 9, sizeof(sc->name));
            } else {
                *item_idx = -1;
            }
        } else if (strncmp(section, "autostart:", 10) == 0) {
            *sec = SEC_AUTOSTART;
            if (conf->autostart_count < STLXDM_CONF_MAX_AUTOSTART) {
                *item_idx = conf->autostart_count++;
                stlxdm_conf_autostart_t* as = &conf->autostart[*item_idx];
                memset(as, 0, sizeof(*as));
            } else {
                *item_idx = -1;
            }
        } else {
            *sec = SEC_NONE;
        }
        return;
    }

    char* eq = strchr(s, '=');
    if (!eq) return;
    *eq = '\0';
    char* key = trim(s);
    char* val = trim(eq + 1);

    switch (*sec) {
    case SEC_DESKTOP:
        if (strcmp(key, "bg_color") == 0)
            conf->bg_color = (uint32_t)strtoul(val, NULL, 16);
        break;

    case SEC_THEME:
        if (strcmp(key, "bar_color") == 0)
            conf->bar_color = (uint32_t)strtoul(val, NULL, 16);
        else if (strcmp(key, "bar_font_size") == 0)
            conf->bar_font_size = (uint32_t)strtoul(val, NULL, 10);
        else if (strcmp(key, "accent_color") == 0)
            conf->accent_color = (uint32_t)strtoul(val, NULL, 16);
        else if (strcmp(key, "text_color") == 0)
            conf->text_color = (uint32_t)strtoul(val, NULL, 16);
        break;

    case SEC_TASKBAR_GLOBAL:
        if (strcmp(key, "height") == 0)
            conf->taskbar_height = (uint32_t)strtoul(val, NULL, 10);
        else if (strcmp(key, "icon_size") == 0)
            conf->taskbar_icon_size = (uint32_t)strtoul(val, NULL, 10);
        else if (strcmp(key, "spacing") == 0)
            conf->taskbar_spacing = (uint32_t)strtoul(val, NULL, 10);
        break;

    case SEC_TASKBAR_ITEM:
        if (*item_idx < 0) break;
        {
            stlxdm_conf_taskbar_item_t* it =
                &conf->taskbar_items[*item_idx];
            if (strcmp(key, "label") == 0)
                safe_copy(it->label, val, sizeof(it->label));
            else if (strcmp(key, "path") == 0)
                safe_copy(it->path, val, sizeof(it->path));
            else if (strcmp(key, "icon") == 0)
                safe_copy(it->icon_path, val, sizeof(it->icon_path));
        }
        break;

    case SEC_SHORTCUT:
        if (*item_idx < 0) break;
        {
            stlxdm_conf_shortcut_t* sc = &conf->shortcuts[*item_idx];
            if (strcmp(key, "key") == 0)
                safe_copy(sc->key, val, sizeof(sc->key));
            else if (strcmp(key, "action") == 0)
                safe_copy(sc->action, val, sizeof(sc->action));
            else if (strcmp(key, "path") == 0)
                safe_copy(sc->path, val, sizeof(sc->path));
        }
        break;

    case SEC_AUTOSTART:
        if (*item_idx < 0) break;
        {
            stlxdm_conf_autostart_t* as = &conf->autostart[*item_idx];
            if (strcmp(key, "path") == 0)
                safe_copy(as->path, val, sizeof(as->path));
        }
        break;

    case SEC_NONE:
        break;
    }
}

int stlxdm_conf_load(stlxdm_config_t* conf, const char* path) {
    stlxdm_conf_defaults(conf);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }

    size_t file_size = (size_t)st.st_size;
    char* data = malloc(file_size + 1);
    if (!data) {
        close(fd);
        return -1;
    }

    size_t total = 0;
    while (total < file_size) {
        ssize_t n = read(fd, data + total, file_size - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    data[total] = '\0';

    section_type_t sec = SEC_NONE;
    int item_idx = -1;

    char* cursor = data;
    while (*cursor) {
        char* eol = strchr(cursor, '\n');
        if (eol) {
            *eol = '\0';
            parse_line(conf, cursor, &sec, &item_idx);
            cursor = eol + 1;
        } else {
            parse_line(conf, cursor, &sec, &item_idx);
            break;
        }
    }

    free(data);
    return 0;
}
