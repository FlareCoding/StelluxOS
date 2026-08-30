/* The INI-style configuration parser. Sections select a state,
 * key=value lines fill the active section, and repeated item
 * sections (taskbar:, shortcut:, autostart:) append to their arrays.
 */
#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

enum class conf_section {
    none,
    desktop,
    theme,
    taskbar,
    input,
    pin,
    shortcut,
    autostart,
};

void dm_config_defaults(dm_config& conf) {
    memset(&conf, 0, sizeof(conf));

    conf.bg_color      = 0xFF2D2D30;
    conf.bar_color     = 0xFF1E1E1E;
    conf.bar_font_size = 14;
    conf.accent_color  = 0xFF888888;
    conf.text_color    = 0xFFCCCCCC;

    conf.taskbar_height    = 48;
    conf.taskbar_icon_size = 32;
    conf.taskbar_spacing   = 8;

    conf.key_repeat_delay_ms    = 400;
    conf.key_repeat_interval_ms = 40;
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
static void enter_section(dm_config& conf, char* name,
                          conf_section& sec, int32_t& item_idx) {
    if (strcmp(name, "desktop") == 0) {
        sec = conf_section::desktop;
    } else if (strcmp(name, "theme") == 0) {
        sec = conf_section::theme;
    } else if (strcmp(name, "taskbar") == 0) {
        sec = conf_section::taskbar;
    } else if (strcmp(name, "input") == 0) {
        sec = conf_section::input;
    } else if (strncmp(name, "taskbar:", 8) == 0) {
        sec = conf_section::pin;
        item_idx = conf.pin_count < DM_CONF_MAX_TASKBAR
                 ? static_cast<int32_t>(conf.pin_count++) : -1;
    } else if (strncmp(name, "shortcut:", 9) == 0) {
        sec = conf_section::shortcut;
        item_idx = conf.shortcut_count < DM_CONF_MAX_SHORTCUTS
                 ? static_cast<int32_t>(conf.shortcut_count++) : -1;
    } else if (strncmp(name, "autostart:", 10) == 0) {
        sec = conf_section::autostart;
        item_idx = conf.autostart_count < DM_CONF_MAX_AUTOSTART
                 ? static_cast<int32_t>(conf.autostart_count++) : -1;
    } else {
        sec = conf_section::none;
    }
}

static void apply_value(dm_config& conf, conf_section sec,
                        int32_t item_idx, const char* key,
                        const char* val) {
    switch (sec) {
    case conf_section::desktop:
        if (strcmp(key, "bg_color") == 0) {
            conf.bg_color = static_cast<uint32_t>(strtoul(val, nullptr, 16));
        } else if (strcmp(key, "wallpaper") == 0) {
            copy_bounded(conf.wallpaper, val, sizeof(conf.wallpaper));
        }
        return;
    case conf_section::theme:
        if (strcmp(key, "bar_color") == 0) {
            conf.bar_color = static_cast<uint32_t>(strtoul(val, nullptr, 16));
        } else if (strcmp(key, "bar_font_size") == 0) {
            conf.bar_font_size = static_cast<uint32_t>(strtoul(val, nullptr, 10));
        } else if (strcmp(key, "accent_color") == 0) {
            conf.accent_color = static_cast<uint32_t>(strtoul(val, nullptr, 16));
        } else if (strcmp(key, "text_color") == 0) {
            conf.text_color = static_cast<uint32_t>(strtoul(val, nullptr, 16));
        }
        return;
    case conf_section::taskbar:
        if (strcmp(key, "height") == 0) {
            conf.taskbar_height = static_cast<uint32_t>(strtoul(val, nullptr, 10));
        } else if (strcmp(key, "icon_size") == 0) {
            conf.taskbar_icon_size = static_cast<uint32_t>(strtoul(val, nullptr, 10));
        } else if (strcmp(key, "spacing") == 0) {
            conf.taskbar_spacing = static_cast<uint32_t>(strtoul(val, nullptr, 10));
        }
        return;
    case conf_section::input:
        if (strcmp(key, "key_repeat_delay_ms") == 0) {
            conf.key_repeat_delay_ms =
                static_cast<uint32_t>(strtoul(val, nullptr, 10));
        } else if (strcmp(key, "key_repeat_interval_ms") == 0) {
            conf.key_repeat_interval_ms =
                static_cast<uint32_t>(strtoul(val, nullptr, 10));
        }
        return;
    case conf_section::pin: {
        if (item_idx < 0) {
            return;
        }

        dm_conf_pin& it = conf.pins[item_idx];
        if (strcmp(key, "label") == 0) {
            copy_bounded(it.label, val, sizeof(it.label));
        } else if (strcmp(key, "path") == 0) {
            copy_bounded(it.path, val, sizeof(it.path));
        } else if (strcmp(key, "icon") == 0) {
            copy_bounded(it.icon_path, val, sizeof(it.icon_path));
        }
        return;
    }
    case conf_section::shortcut: {
        if (item_idx < 0) {
            return;
        }

        dm_conf_shortcut& sc = conf.shortcuts[item_idx];
        if (strcmp(key, "key") == 0) {
            copy_bounded(sc.key, val, sizeof(sc.key));
        } else if (strcmp(key, "action") == 0) {
            copy_bounded(sc.action, val, sizeof(sc.action));
        } else if (strcmp(key, "path") == 0) {
            copy_bounded(sc.path, val, sizeof(sc.path));
        }
        return;
    }
    case conf_section::autostart: {
        if (item_idx < 0) {
            return;
        }

        dm_conf_autostart& as = conf.autostart[item_idx];
        if (strcmp(key, "path") == 0) {
            copy_bounded(as.path, val, sizeof(as.path));
        } else if (strcmp(key, "args") == 0) {
            copy_bounded(as.args, val, sizeof(as.args));
        }
        return;
    }
    case conf_section::none:
        return;
    }
}

static void parse_line(dm_config& conf, const char* line,
                       conf_section& sec, int32_t& item_idx) {
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
    apply_value(conf, sec, item_idx, trim(s), trim(eq + 1));
}

int dm_config_load(dm_config& conf, const char* path) {
    dm_config_defaults(conf);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    char* data = static_cast<char*>(malloc(file_size + 1));
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
        total += static_cast<size_t>(n);
    }
    close(fd);
    data[total] = '\0';

    conf_section sec = conf_section::none;
    int32_t item_idx = -1;
    char* cursor = data;
    while (*cursor) {
        char* eol = strchr(cursor, '\n');
        if (eol) {
            *eol = '\0';
            parse_line(conf, cursor, sec, item_idx);
            cursor = eol + 1;
        } else {
            parse_line(conf, cursor, sec, item_idx);
            break;
        }
    }

    free(data);
    return 0;
}
