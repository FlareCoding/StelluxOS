/* The INI-style configuration parser and serializer. Sections select
 * a state, key=value lines fill the active section, and repeated item
 * sections (taskbar:, shortcut:, autostart:) append to their arrays.
 */
#include <stlxconf/conf.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum {
    SEC_NONE,
    SEC_DESKTOP,
    SEC_THEME,
    SEC_TASKBAR,
    SEC_INPUT,
    SEC_PIN,
    SEC_SHORTCUT,
    SEC_AUTOSTART,
} conf_section_t;

void stlxconf_defaults(stlxconf_t* conf) {
    memset(conf, 0, sizeof(*conf));

    conf->bg_color      = 0xFF2D2D30;
    conf->bar_color     = 0xFF1E1E1E;
    conf->bar_font_size = 14;
    conf->accent_color  = 0xFF888888;
    conf->text_color    = 0xFFCCCCCC;

    conf->taskbar_height    = 48;
    conf->taskbar_icon_size = 32;
    conf->taskbar_spacing   = 8;

    conf->key_repeat_delay_ms    = 400;
    conf->key_repeat_interval_ms = 40;
}

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }

    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';

    return s;
}

static void copy_bounded(char* dst, const char* src, size_t cap) {
    size_t len = strlen(src);
    if (len >= cap) {
        len = cap - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Section headers switch state and allocate item slots. item_idx is
 * negative when the item table is full and values must be dropped. */
static void enter_section(stlxconf_t* conf, char* name,
                          conf_section_t* sec, int32_t* item_idx) {
    if (strcmp(name, "desktop") == 0) {
        *sec = SEC_DESKTOP;
    } else if (strcmp(name, "theme") == 0) {
        *sec = SEC_THEME;
    } else if (strcmp(name, "taskbar") == 0) {
        *sec = SEC_TASKBAR;
    } else if (strcmp(name, "input") == 0) {
        *sec = SEC_INPUT;
    } else if (strncmp(name, "taskbar:", 8) == 0) {
        *sec = SEC_PIN;
        *item_idx = conf->pin_count < STLXCONF_MAX_TASKBAR
                  ? (int32_t)conf->pin_count++ : -1;
        if (*item_idx >= 0) {
            copy_bounded(conf->pins[*item_idx].name, name + 8,
                         sizeof(conf->pins[*item_idx].name));
        }
    } else if (strncmp(name, "shortcut:", 9) == 0) {
        *sec = SEC_SHORTCUT;
        *item_idx = conf->shortcut_count < STLXCONF_MAX_SHORTCUTS
                  ? (int32_t)conf->shortcut_count++ : -1;
        if (*item_idx >= 0) {
            copy_bounded(conf->shortcuts[*item_idx].name, name + 9,
                         sizeof(conf->shortcuts[*item_idx].name));
        }
    } else if (strncmp(name, "autostart:", 10) == 0) {
        *sec = SEC_AUTOSTART;
        *item_idx = conf->autostart_count < STLXCONF_MAX_AUTOSTART
                  ? (int32_t)conf->autostart_count++ : -1;
        if (*item_idx >= 0) {
            copy_bounded(conf->autostart[*item_idx].name, name + 10,
                         sizeof(conf->autostart[*item_idx].name));
        }
    } else {
        *sec = SEC_NONE;
    }
}

static void apply_value(stlxconf_t* conf, conf_section_t sec,
                        int32_t item_idx, const char* key,
                        const char* val) {
    switch (sec) {
    case SEC_DESKTOP:
        if (strcmp(key, "bg_color") == 0) {
            conf->bg_color = (uint32_t)strtoul(val, NULL, 16);
        } else if (strcmp(key, "wallpaper") == 0) {
            copy_bounded(conf->wallpaper, val, sizeof(conf->wallpaper));
        }
        return;
    case SEC_THEME:
        if (strcmp(key, "bar_color") == 0) {
            conf->bar_color = (uint32_t)strtoul(val, NULL, 16);
        } else if (strcmp(key, "bar_font_size") == 0) {
            conf->bar_font_size = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "accent_color") == 0) {
            conf->accent_color = (uint32_t)strtoul(val, NULL, 16);
        } else if (strcmp(key, "text_color") == 0) {
            conf->text_color = (uint32_t)strtoul(val, NULL, 16);
        }
        return;
    case SEC_TASKBAR:
        if (strcmp(key, "height") == 0) {
            conf->taskbar_height = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "icon_size") == 0) {
            conf->taskbar_icon_size = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "spacing") == 0) {
            conf->taskbar_spacing = (uint32_t)strtoul(val, NULL, 10);
        }
        return;
    case SEC_INPUT:
        if (strcmp(key, "key_repeat_delay_ms") == 0) {
            conf->key_repeat_delay_ms = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "key_repeat_interval_ms") == 0) {
            conf->key_repeat_interval_ms = (uint32_t)strtoul(val, NULL, 10);
        }
        return;
    case SEC_PIN: {
        if (item_idx < 0) {
            return;
        }

        stlxconf_pin_t* it = &conf->pins[item_idx];
        if (strcmp(key, "label") == 0) {
            copy_bounded(it->label, val, sizeof(it->label));
        } else if (strcmp(key, "path") == 0) {
            copy_bounded(it->path, val, sizeof(it->path));
        } else if (strcmp(key, "icon") == 0) {
            copy_bounded(it->icon_path, val, sizeof(it->icon_path));
        }
        return;
    }
    case SEC_SHORTCUT: {
        if (item_idx < 0) {
            return;
        }

        stlxconf_shortcut_t* sc = &conf->shortcuts[item_idx];
        if (strcmp(key, "key") == 0) {
            copy_bounded(sc->key, val, sizeof(sc->key));
        } else if (strcmp(key, "action") == 0) {
            copy_bounded(sc->action, val, sizeof(sc->action));
        } else if (strcmp(key, "path") == 0) {
            copy_bounded(sc->path, val, sizeof(sc->path));
        }
        return;
    }
    case SEC_AUTOSTART: {
        if (item_idx < 0) {
            return;
        }

        stlxconf_autostart_t* as = &conf->autostart[item_idx];
        if (strcmp(key, "path") == 0) {
            copy_bounded(as->path, val, sizeof(as->path));
        } else if (strcmp(key, "args") == 0) {
            copy_bounded(as->args, val, sizeof(as->args));
        }
        return;
    }
    case SEC_NONE:
        return;
    }
}

static void parse_line(stlxconf_t* conf, const char* line,
                       conf_section_t* sec, int32_t* item_idx) {
    char buf[512];
    copy_bounded(buf, line, sizeof(buf));
    char* s = trim(buf);

    if (*s == '\0' || *s == '#' || *s == ';') {
        return;
    }

    if (*s == '[') {
        char* end = strchr(s, ']');
        if (!end) {
            return;
        }

        *end = '\0';
        enter_section(conf, s + 1, sec, item_idx);
        return;
    }

    char* eq = strchr(s, '=');
    if (!eq) {
        return;
    }

    *eq = '\0';
    apply_value(conf, *sec, *item_idx, trim(s), trim(eq + 1));
}

int stlxconf_load(stlxconf_t* conf, const char* path) {
    stlxconf_defaults(conf);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

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
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    close(fd);
    data[total] = '\0';

    conf_section_t sec = SEC_NONE;
    int32_t item_idx = -1;
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

int stlxconf_save(const stlxconf_t* conf, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) {
        return -1;
    }

    fprintf(f, "[desktop]\n");
    fprintf(f, "bg_color=0x%08X\n", conf->bg_color);
    fprintf(f, "wallpaper=%s\n", conf->wallpaper);

    fprintf(f, "\n[theme]\n");
    fprintf(f, "bar_color=0x%08X\n", conf->bar_color);
    fprintf(f, "bar_font_size=%u\n", conf->bar_font_size);
    fprintf(f, "accent_color=0x%08X\n", conf->accent_color);
    fprintf(f, "text_color=0x%08X\n", conf->text_color);

    fprintf(f, "\n[taskbar]\n");
    fprintf(f, "height=%u\n", conf->taskbar_height);
    fprintf(f, "icon_size=%u\n", conf->taskbar_icon_size);
    fprintf(f, "spacing=%u\n", conf->taskbar_spacing);

    fprintf(f, "\n[input]\n");
    fprintf(f, "key_repeat_delay_ms=%u\n", conf->key_repeat_delay_ms);
    fprintf(f, "key_repeat_interval_ms=%u\n", conf->key_repeat_interval_ms);

    for (uint32_t i = 0; i < conf->shortcut_count; i++) {
        const stlxconf_shortcut_t* sc = &conf->shortcuts[i];
        fprintf(f, "\n[shortcut:%s]\n", sc->name);
        fprintf(f, "key=%s\n", sc->key);
        fprintf(f, "action=%s\n", sc->action);
        fprintf(f, "path=%s\n", sc->path);
    }

    for (uint32_t i = 0; i < conf->pin_count; i++) {
        const stlxconf_pin_t* it = &conf->pins[i];
        fprintf(f, "\n[taskbar:%s]\n", it->name);
        fprintf(f, "label=%s\n", it->label);
        fprintf(f, "path=%s\n", it->path);
        fprintf(f, "icon=%s\n", it->icon_path);
    }

    for (uint32_t i = 0; i < conf->autostart_count; i++) {
        const stlxconf_autostart_t* as = &conf->autostart[i];
        fprintf(f, "\n[autostart:%s]\n", as->name);
        fprintf(f, "path=%s\n", as->path);
        if (as->args[0]) {
            fprintf(f, "args=%s\n", as->args);
        }
    }

    fclose(f);
    return 0;
}
