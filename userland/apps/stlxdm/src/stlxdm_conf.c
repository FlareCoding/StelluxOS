#include "stlxdm_conf.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void stlxdm_conf_init(stlxdm_conf_t* conf) {
    memset(conf, 0, sizeof(*conf));
}

static char* strip(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static uint32_t parse_hex_color(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    return (uint32_t)strtoul(s, NULL, 16);
}

typedef enum {
    SEC_NONE,
    SEC_DESKTOP,
    SEC_THEME,
    SEC_TASKBAR,
    SEC_SHORTCUT,
    SEC_UNKNOWN
} section_kind_t;

static section_kind_t parse_section(const char* header, char* name_out,
                                     int name_max) {
    name_out[0] = '\0';

    if (strncmp(header, "desktop", 7) == 0 && header[7] == '\0')
        return SEC_DESKTOP;
    if (strncmp(header, "theme", 5) == 0 && header[5] == '\0')
        return SEC_THEME;

    if (strncmp(header, "taskbar:", 8) == 0) {
        const char* n = header + 8;
        while (*n && isspace((unsigned char)*n)) n++;
        int len = (int)strlen(n);
        if (len >= name_max) len = name_max - 1;
        memcpy(name_out, n, len);
        name_out[len] = '\0';
        return SEC_TASKBAR;
    }

    if (strncmp(header, "shortcut:", 9) == 0) {
        const char* n = header + 9;
        while (*n && isspace((unsigned char)*n)) n++;
        int len = (int)strlen(n);
        if (len >= name_max) len = name_max - 1;
        memcpy(name_out, n, len);
        name_out[len] = '\0';
        return SEC_SHORTCUT;
    }

    return SEC_UNKNOWN;
}

static void apply_kv(stlxdm_conf_t* conf, section_kind_t sec,
                      const char* key, const char* val) {
    switch (sec) {
    case SEC_DESKTOP:
        if (strcmp(key, "wallpaper") == 0) {
            strncpy(conf->wallpaper, val, sizeof(conf->wallpaper) - 1);
        } else if (strcmp(key, "bg_color") == 0) {
            conf->bg_color = parse_hex_color(val);
            conf->has_bg_color = 1;
        }
        break;
    case SEC_THEME:
        if (strcmp(key, "bar_color") == 0) {
            conf->bar_color = parse_hex_color(val);
            conf->has_bar_color = 1;
        } else if (strcmp(key, "bar_font_size") == 0) {
            conf->bar_font_size = (uint32_t)atoi(val);
            conf->has_bar_font_size = 1;
        } else if (strcmp(key, "accent_color") == 0) {
            conf->accent_color = parse_hex_color(val);
            conf->has_accent_color = 1;
        }
        break;
    case SEC_TASKBAR: {
        if (conf->taskbar_count <= 0) break;
        stlxdm_conf_taskbar_entry_t* e =
            &conf->taskbar[conf->taskbar_count - 1];
        if (strcmp(key, "label") == 0) {
            strncpy(e->label, val, sizeof(e->label) - 1);
        } else if (strcmp(key, "path") == 0) {
            strncpy(e->path, val, sizeof(e->path) - 1);
        } else if (strcmp(key, "icon") == 0) {
            strncpy(e->icon, val, sizeof(e->icon) - 1);
        }
        break;
    }
    case SEC_SHORTCUT: {
        if (conf->shortcut_count <= 0) break;
        stlxdm_conf_shortcut_t* s =
            &conf->shortcuts[conf->shortcut_count - 1];
        if (strcmp(key, "key") == 0) {
            strncpy(s->key, val, sizeof(s->key) - 1);
        } else if (strcmp(key, "action") == 0) {
            strncpy(s->action, val, sizeof(s->action) - 1);
        } else if (strcmp(key, "path") == 0) {
            strncpy(s->path, val, sizeof(s->path) - 1);
        }
        break;
    }
    default:
        break;
    }
}

int stlxdm_conf_load(stlxdm_conf_t* conf, const char* path) {
    stlxdm_conf_init(conf);

    FILE* f = fopen(path, "r");
    if (!f) {
        printf("stlxdm: cannot open config %s\r\n", path);
        return -1;
    }

    char line[512];
    section_kind_t cur_sec = SEC_NONE;

    while (fgets(line, (int)sizeof(line), f)) {
        char* s = strip(line);
        if (*s == '\0' || *s == '#') continue;

        if (*s == '[') {
            char* close = strchr(s, ']');
            if (!close) continue;
            *close = '\0';
            char* header = strip(s + 1);
            char sec_name[64];
            cur_sec = parse_section(header, sec_name, (int)sizeof(sec_name));

            if (cur_sec == SEC_TASKBAR &&
                conf->taskbar_count < STLXDM_CONF_MAX_TASKBAR) {
                stlxdm_conf_taskbar_entry_t* e =
                    &conf->taskbar[conf->taskbar_count++];
                memset(e, 0, sizeof(*e));
                strncpy(e->label, sec_name, sizeof(e->label) - 1);
            } else if (cur_sec == SEC_SHORTCUT &&
                       conf->shortcut_count < STLXDM_CONF_MAX_SHORTCUTS) {
                stlxdm_conf_shortcut_t* sc =
                    &conf->shortcuts[conf->shortcut_count++];
                memset(sc, 0, sizeof(*sc));
                strncpy(sc->name, sec_name, sizeof(sc->name) - 1);
            }
            continue;
        }

        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = strip(s);
        char* val = strip(eq + 1);
        apply_kv(conf, cur_sec, key, val);
    }

    fclose(f);
    printf("stlxdm: loaded config from %s (%d taskbar entries)\r\n",
           path, conf->taskbar_count);
    return 0;
}
