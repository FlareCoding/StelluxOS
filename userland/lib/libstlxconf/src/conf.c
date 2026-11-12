/* The INI-style configuration parser and serializer. Sections select
 * a state, key=value lines fill the active section, and repeated item
 * sections, named prefix:name such as taskbar:files, append to arrays.
 */
#include <stlxconf/conf.h>
#include <stlxtheme/theme.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum {
    SEC_NONE,
    SEC_DESKTOP,
    SEC_THEME,
    SEC_INPUT,
    SEC_QUICK_ACTIONS,
    SEC_PIN,
    SEC_SHORTCUT,
    SEC_AUTOSTART,
    SEC_URL_HANDLER,
    SEC_FILE_HANDLER,
    SEC_ACTION,
} conf_section_t;

static const char* const COMPOSITOR_NAMES[] = {
    [STLXCONF_COMPOSITOR_AUTO] = "auto",
    [STLXCONF_COMPOSITOR_CPU]  = "cpu",
};

static const char* const COMPOSE_FAULT_NAMES[] = {
    [STLXCONF_COMPOSE_FAULT_NONE]  = "none",
    [STLXCONF_COMPOSE_FAULT_FAULT] = "fault",
    [STLXCONF_COMPOSE_FAULT_HANG]  = "hang",
};

static const char* const GLASS_LEVEL_NAMES[] = {
    [STLXCONF_GLASS_LEVEL_SOLID]  = "solid",
    [STLXCONF_GLASS_LEVEL_SUBTLE] = "subtle",
    [STLXCONF_GLASS_LEVEL_GLASS]  = "glass",
    [STLXCONF_GLASS_LEVEL_CLEAR]  = "clear",
};

static const char* const GLASS_TINT_NAMES[] = {
    [STLXCONF_GLASS_TINT_WARM]    = "warm",
    [STLXCONF_GLASS_TINT_NEUTRAL] = "neutral",
};

static const char* const SWITCH_NAMES[] = { "off", "on" };

static const char* const ACTION_NAMES[] = {
    [STLXCONF_ACTION_UNKNOWN]       = "",
    [STLXCONF_ACTION_EXEC]          = "exec",
    [STLXCONF_ACTION_POWER]         = "power",
    [STLXCONF_ACTION_QUICK_ACTIONS] = "quick_actions",
    [STLXCONF_ACTION_PASTE]         = "paste",
};

/* What people read for stlxdm's own actions, where exec is named by its program instead */
static const char* const ACTION_LABELS[] = {
    [STLXCONF_ACTION_UNKNOWN]       = NULL,
    [STLXCONF_ACTION_EXEC]          = NULL,
    [STLXCONF_ACTION_POWER]         = "Power",
    [STLXCONF_ACTION_QUICK_ACTIONS] = "Quick Actions",
    [STLXCONF_ACTION_PASTE]         = "Paste",
};

#define NAME_COUNT(names) ((int)(sizeof(names) / sizeof((names)[0])))

void stlxconf_defaults(stlxconf_t* conf) {
    memset(conf, 0, sizeof(*conf));

    conf->bg_color      = STLXTHEME_WINDOW_BG;
    conf->bar_color     = 0xFF1E1E1E;
    conf->bar_font_size = 14;
    conf->text_color    = 0xFFCCCCCC;

    conf->glass_level = STLXCONF_GLASS_LEVEL_GLASS;
    conf->glass_tint  = STLXCONF_GLASS_TINT_WARM;

    conf->key_repeat_delay_ms    = STLXCONF_DEFAULT_KEY_REPEAT_DELAY_MS;
    conf->key_repeat_interval_ms = STLXCONF_DEFAULT_KEY_REPEAT_INTERVAL_MS;

    strcpy(conf->quick_actions_skip, STLXCONF_DEFAULT_QUICK_ACTIONS_SKIP);
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

/* The index of `val` in `names`, or -1 when it is none of them */
static int find_name(const char* const* names, int count, const char* val) {
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], val) == 0) {
            return i;
        }
    }

    return -1;
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
                          conf_section_t* sec, int32_t* item_idx,
                          const char* source_path, bool from_drop_in) {
    /* A drop-in file may only add items, so its other sections are skipped */
    if (strchr(name, ':') == NULL && from_drop_in) {
        *sec = SEC_NONE;
    } else if (strcmp(name, "desktop") == 0) {
        *sec = SEC_DESKTOP;
    } else if (strcmp(name, "theme") == 0) {
        *sec = SEC_THEME;
    } else if (strcmp(name, "input") == 0) {
        *sec = SEC_INPUT;
    } else if (strcmp(name, "quick_actions") == 0) {
        *sec = SEC_QUICK_ACTIONS;
    } else if (strncmp(name, "taskbar:", 8) == 0) {
        *sec = SEC_PIN;
        *item_idx = conf->pin_count < STLXCONF_MAX_TASKBAR
                  ? (int32_t)conf->pin_count++ : -1;
        if (*item_idx >= 0) {
            copy_bounded(conf->pins[*item_idx].name, name + 8,
                         sizeof(conf->pins[*item_idx].name));
            copy_bounded(conf->pins[*item_idx].source_path, source_path,
                         sizeof(conf->pins[*item_idx].source_path));
            conf->pins[*item_idx].from_drop_in = from_drop_in;
        }
    } else if (strncmp(name, "shortcut:", 9) == 0) {
        *sec = SEC_SHORTCUT;
        *item_idx = conf->shortcut_count < STLXCONF_MAX_SHORTCUTS
                  ? (int32_t)conf->shortcut_count++ : -1;
        if (*item_idx >= 0) {
            copy_bounded(conf->shortcuts[*item_idx].name, name + 9,
                         sizeof(conf->shortcuts[*item_idx].name));
            conf->shortcuts[*item_idx].from_drop_in = from_drop_in;
        }
    } else if (strncmp(name, "autostart:", 10) == 0) {
        *sec = SEC_AUTOSTART;
        *item_idx = conf->autostart_count < STLXCONF_MAX_AUTOSTART
                  ? (int32_t)conf->autostart_count++ : -1;
        if (*item_idx >= 0) {
            copy_bounded(conf->autostart[*item_idx].name, name + 10,
                         sizeof(conf->autostart[*item_idx].name));
            conf->autostart[*item_idx].from_drop_in = from_drop_in;
        }
    } else if (strncmp(name, "url_handler:", 12) == 0) {
        *sec = SEC_URL_HANDLER;
        *item_idx = conf->url_handler_count < STLXCONF_MAX_URL_HANDLERS
                  ? (int32_t)conf->url_handler_count++ : -1;
        if (*item_idx >= 0) {
            copy_bounded(conf->url_handlers[*item_idx].scheme, name + 12,
                         sizeof(conf->url_handlers[*item_idx].scheme));
            conf->url_handlers[*item_idx].from_drop_in = from_drop_in;
        }
    } else if (strncmp(name, "file_handler:", 13) == 0) {
        *sec = SEC_FILE_HANDLER;
        *item_idx = conf->file_handler_count < STLXCONF_MAX_FILE_HANDLERS
                  ? (int32_t)conf->file_handler_count++ : -1;
        if (*item_idx >= 0) {
            copy_bounded(conf->file_handlers[*item_idx].mime_type, name + 13,
                         sizeof(conf->file_handlers[*item_idx].mime_type));
            conf->file_handlers[*item_idx].from_drop_in = from_drop_in;
        }
    } else if (strncmp(name, "action:", 7) == 0) {
        *sec = SEC_ACTION;
        *item_idx = conf->action_count < STLXCONF_MAX_ACTIONS
                  ? (int32_t)conf->action_count++ : -1;
        if (*item_idx >= 0) {
            copy_bounded(conf->actions[*item_idx].name, name + 7,
                         sizeof(conf->actions[*item_idx].name));
            copy_bounded(conf->actions[*item_idx].source_path, source_path,
                         sizeof(conf->actions[*item_idx].source_path));
            conf->actions[*item_idx].from_drop_in = from_drop_in;
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
        } else if (strcmp(key, "compositor") == 0) {
            int found = find_name(COMPOSITOR_NAMES, NAME_COUNT(COMPOSITOR_NAMES), val);
            if (found >= 0) {
                conf->compositor = (stlxconf_compositor_t)found;
            }
        } else if (strcmp(key, "compare_frames") == 0) {
            int found = find_name(SWITCH_NAMES, NAME_COUNT(SWITCH_NAMES), val);
            if (found >= 0) {
                conf->compare_frames = found == 1;
            }
        } else if (strcmp(key, "compose_fault") == 0) {
            int found = find_name(COMPOSE_FAULT_NAMES, NAME_COUNT(COMPOSE_FAULT_NAMES), val);
            if (found >= 0) {
                conf->compose_fault = (stlxconf_compose_fault_t)found;
            }
        }
        return;
    case SEC_THEME:
        if (strcmp(key, "bar_color") == 0) {
            conf->bar_color = (uint32_t)strtoul(val, NULL, 16);
        } else if (strcmp(key, "bar_font_size") == 0) {
            conf->bar_font_size = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "text_color") == 0) {
            conf->text_color = (uint32_t)strtoul(val, NULL, 16);
        } else if (strcmp(key, "glass_level") == 0) {
            int found = find_name(GLASS_LEVEL_NAMES, NAME_COUNT(GLASS_LEVEL_NAMES), val);
            if (found >= 0) {
                conf->glass_level = (stlxconf_glass_level_t)found;
            }
        } else if (strcmp(key, "glass_tint") == 0) {
            int found = find_name(GLASS_TINT_NAMES, NAME_COUNT(GLASS_TINT_NAMES), val);
            if (found >= 0) {
                conf->glass_tint = (stlxconf_glass_tint_t)found;
            }
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
        } else if (strcmp(key, "args") == 0) {
            copy_bounded(it->args, val, sizeof(it->args));
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
        } else if (strcmp(key, "args") == 0) {
            copy_bounded(sc->args, val, sizeof(sc->args));
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
    case SEC_URL_HANDLER: {
        if (item_idx < 0) {
            return;
        }

        stlxconf_url_handler_t* uh = &conf->url_handlers[item_idx];
        if (strcmp(key, "path") == 0) {
            copy_bounded(uh->path, val, sizeof(uh->path));
        } else if (strcmp(key, "args") == 0) {
            copy_bounded(uh->args, val, sizeof(uh->args));
        }
        return;
    }
    case SEC_FILE_HANDLER: {
        if (item_idx < 0) {
            return;
        }

        stlxconf_file_handler_t* fh = &conf->file_handlers[item_idx];
        if (strcmp(key, "path") == 0) {
            copy_bounded(fh->path, val, sizeof(fh->path));
        } else if (strcmp(key, "args") == 0) {
            copy_bounded(fh->args, val, sizeof(fh->args));
        }

        return;
    }
    case SEC_ACTION: {
        if (item_idx < 0) {
            return;
        }

        stlxconf_action_t* ac = &conf->actions[item_idx];
        if (strcmp(key, "label") == 0) {
            copy_bounded(ac->label, val, sizeof(ac->label));
        } else if (strcmp(key, "subtitle") == 0) {
            copy_bounded(ac->subtitle, val, sizeof(ac->subtitle));
        } else if (strcmp(key, "keywords") == 0) {
            copy_bounded(ac->keywords, val, sizeof(ac->keywords));
        } else if (strcmp(key, "icon") == 0) {
            copy_bounded(ac->icon_path, val, sizeof(ac->icon_path));
        } else if (strcmp(key, "action") == 0) {
            copy_bounded(ac->action, val, sizeof(ac->action));
        } else if (strcmp(key, "path") == 0) {
            copy_bounded(ac->path, val, sizeof(ac->path));
        } else if (strcmp(key, "args") == 0) {
            copy_bounded(ac->args, val, sizeof(ac->args));
        }

        return;
    }
    case SEC_QUICK_ACTIONS:
        if (strcmp(key, "skip") == 0) {
            copy_bounded(conf->quick_actions_skip, val, sizeof(conf->quick_actions_skip));
        }

        return;
    case SEC_NONE:
        return;
    }
}

static void parse_line(stlxconf_t* conf, const char* line,
                       conf_section_t* sec, int32_t* item_idx,
                       const char* source_path, bool from_drop_in) {
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
        enter_section(conf, s + 1, sec, item_idx, source_path, from_drop_in);
        return;
    }

    char* eq = strchr(s, '=');
    if (!eq) {
        return;
    }

    *eq = '\0';
    apply_value(conf, *sec, *item_idx, trim(s), trim(eq + 1));
}

/* Parses one file over the configuration, marking the items it adds */
static int parse_file(stlxconf_t* conf, const char* path, bool from_drop_in) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }

    /* An empty file has nothing to say and is not an error */
    if (st.st_size == 0) {
        close(fd);
        return 0;
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
            parse_line(conf, cursor, &sec, &item_idx, path, from_drop_in);
            cursor = eol + 1;
        } else {
            parse_line(conf, cursor, &sec, &item_idx, path, from_drop_in);
            break;
        }
    }

    free(data);
    return 0;
}

int stlxconf_load(stlxconf_t* conf, const char* path) {
    stlxconf_defaults(conf);

    return parse_file(conf, path, false);
}

static int is_conf_file(const struct dirent* entry) {
    const char* dot = strrchr(entry->d_name, '.');

    return dot && strcmp(dot, ".conf") == 0;
}

int stlxconf_load_drop_ins(stlxconf_t* conf, const char* dir) {
    struct dirent** entries = NULL;
    int count = scandir(dir, &entries, is_conf_file, alphasort);
    if (count < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    int result = 0;
    for (int i = 0; i < count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, entries[i]->d_name);
        if (parse_file(conf, path, true) != 0) {
            result = -1;
        }

        free(entries[i]);
    }

    free(entries);
    return result;
}

int stlxconf_save(const stlxconf_t* conf, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) {
        return -1;
    }

    fprintf(f, "[desktop]\n");
    fprintf(f, "bg_color=0x%08X\n", conf->bg_color);
    fprintf(f, "wallpaper=%s\n", conf->wallpaper);
    fprintf(f, "compositor=%s\n", COMPOSITOR_NAMES[conf->compositor]);
    fprintf(f, "compare_frames=%s\n", SWITCH_NAMES[conf->compare_frames ? 1 : 0]);
    fprintf(f, "compose_fault=%s\n", COMPOSE_FAULT_NAMES[conf->compose_fault]);

    fprintf(f, "\n[theme]\n");
    fprintf(f, "bar_color=0x%08X\n", conf->bar_color);
    fprintf(f, "bar_font_size=%u\n", conf->bar_font_size);
    fprintf(f, "text_color=0x%08X\n", conf->text_color);

    fprintf(f, "glass_level=%s\n", GLASS_LEVEL_NAMES[conf->glass_level]);
    fprintf(f, "glass_tint=%s\n", GLASS_TINT_NAMES[conf->glass_tint]);

    fprintf(f, "\n[input]\n");
    fprintf(f, "key_repeat_delay_ms=%u\n", conf->key_repeat_delay_ms);
    fprintf(f, "key_repeat_interval_ms=%u\n", conf->key_repeat_interval_ms);

    fprintf(f, "\n[quick_actions]\n");
    fprintf(f, "skip=%s\n", conf->quick_actions_skip);

    for (uint32_t i = 0; i < conf->shortcut_count; i++) {
        const stlxconf_shortcut_t* sc = &conf->shortcuts[i];
        if (sc->from_drop_in) {
            continue;
        }

        fprintf(f, "\n[shortcut:%s]\n", sc->name);
        fprintf(f, "key=%s\n", sc->key);
        fprintf(f, "action=%s\n", sc->action);
        fprintf(f, "path=%s\n", sc->path);
        if (sc->args[0]) {
            fprintf(f, "args=%s\n", sc->args);
        }
    }

    for (uint32_t i = 0; i < conf->pin_count; i++) {
        const stlxconf_pin_t* it = &conf->pins[i];
        if (it->from_drop_in) {
            continue;
        }

        fprintf(f, "\n[taskbar:%s]\n", it->name);
        fprintf(f, "label=%s\n", it->label);
        fprintf(f, "path=%s\n", it->path);
        if (it->args[0]) {
            fprintf(f, "args=%s\n", it->args);
        }

        fprintf(f, "icon=%s\n", it->icon_path);
    }

    for (uint32_t i = 0; i < conf->action_count; i++) {
        const stlxconf_action_t* ac = &conf->actions[i];
        if (ac->from_drop_in) {
            continue;
        }

        fprintf(f, "\n[action:%s]\n", ac->name);
        fprintf(f, "label=%s\n", ac->label);
        if (ac->subtitle[0]) {
            fprintf(f, "subtitle=%s\n", ac->subtitle);
        }

        if (ac->keywords[0]) {
            fprintf(f, "keywords=%s\n", ac->keywords);
        }

        if (ac->icon_path[0]) {
            fprintf(f, "icon=%s\n", ac->icon_path);
        }

        fprintf(f, "action=%s\n", ac->action);
        if (ac->path[0]) {
            fprintf(f, "path=%s\n", ac->path);
        }

        if (ac->args[0]) {
            fprintf(f, "args=%s\n", ac->args);
        }
    }

    for (uint32_t i = 0; i < conf->autostart_count; i++) {
        const stlxconf_autostart_t* as = &conf->autostart[i];
        if (as->from_drop_in) {
            continue;
        }

        fprintf(f, "\n[autostart:%s]\n", as->name);
        fprintf(f, "path=%s\n", as->path);
        if (as->args[0]) {
            fprintf(f, "args=%s\n", as->args);
        }
    }

    for (uint32_t i = 0; i < conf->url_handler_count; i++) {
        const stlxconf_url_handler_t* uh = &conf->url_handlers[i];
        if (uh->from_drop_in) {
            continue;
        }

        fprintf(f, "\n[url_handler:%s]\n", uh->scheme);
        fprintf(f, "path=%s\n", uh->path);
        if (uh->args[0]) {
            fprintf(f, "args=%s\n", uh->args);
        }
    }

    for (uint32_t i = 0; i < conf->file_handler_count; i++) {
        const stlxconf_file_handler_t* fh = &conf->file_handlers[i];
        if (fh->from_drop_in) {
            continue;
        }

        fprintf(f, "\n[file_handler:%s]\n", fh->mime_type);
        fprintf(f, "path=%s\n", fh->path);
        if (fh->args[0]) {
            fprintf(f, "args=%s\n", fh->args);
        }
    }

    fclose(f);
    return 0;
}

const char* stlxconf_glass_level_name(stlxconf_glass_level_t level) {
    return GLASS_LEVEL_NAMES[level];
}

const char* stlxconf_glass_tint_name(stlxconf_glass_tint_t tint) {
    return GLASS_TINT_NAMES[tint];
}

const stlxconf_url_handler_t* stlxconf_find_url_handler(const stlxconf_t* conf,
                                                        const char* scheme) {
    for (uint32_t i = 0; i < conf->url_handler_count; i++) {
        if (strcasecmp(conf->url_handlers[i].scheme, scheme) == 0) {
            return &conf->url_handlers[i];
        }
    }

    return NULL;
}

const stlxconf_file_handler_t* stlxconf_find_file_handler(const stlxconf_t* conf,
                                                          const char* mime_type) {
    for (uint32_t i = 0; i < conf->file_handler_count; i++) {
        if (strcasecmp(conf->file_handlers[i].mime_type, mime_type) == 0) {
            return &conf->file_handlers[i];
        }
    }

    return NULL;
}

stlxconf_action_kind_t stlxconf_action_kind(const char* action) {
    int found = find_name(ACTION_NAMES, NAME_COUNT(ACTION_NAMES), action);
    return found > 0 ? (stlxconf_action_kind_t)found : STLXCONF_ACTION_UNKNOWN;
}

const char* stlxconf_action_label(stlxconf_action_kind_t kind) {
    return ACTION_LABELS[kind];
}

int stlxconf_split_args(char* args, const char* argv[], int capacity) {
    int argc = 0;
    char* p = args;
    while (*p && argc < capacity - 1) {
        while (*p == ' ') {
            p++;
        }

        if (!*p) {
            break;
        }

        argv[argc++] = p;
        while (*p && *p != ' ') {
            p++;
        }

        if (*p) {
            *p++ = '\0';
        }
    }

    argv[argc] = NULL;
    return argc;
}
