#include "config.h"
#include <stdio.h>
#include <string.h>

#define CONFIG_FILE_MAX 4096
#define CONFIG_NAME_MAX 64

void config_defaults(term_config_t *config) {
    config->copy_on_select = 0;
    config->paste_protection = 1;
}

// Copies [from, to) into out without the blanks around it
static void copy_trimmed(char *out, const char *from, const char *to) {
    while (from < to && (*from == ' ' || *from == '\t')) {
        from++;
    }

    while (to > from && (to[-1] == ' ' || to[-1] == '\t' || to[-1] == '\r')) {
        to--;
    }

    size_t len = (size_t)(to - from) < CONFIG_NAME_MAX - 1 ? (size_t)(to - from) : CONFIG_NAME_MAX - 1;
    memcpy(out, from, len);
    out[len] = '\0';
}

static void set_flag(int *flag, const char *value) {
    if (strcmp(value, "true") == 0) {
        *flag = 1;
    } else if (strcmp(value, "false") == 0) {
        *flag = 0;
    }
}

void config_parse(term_config_t *config, const char *text, size_t len) {
    char section[CONFIG_NAME_MAX] = "";
    const char *end = text + len;
    for (const char *line = text; line < end;) {
        const char *line_end = memchr(line, '\n', (size_t)(end - line));
        if (!line_end) {
            line_end = end;
        }

        char entry[CONFIG_NAME_MAX];
        copy_trimmed(entry, line, line_end);
        const char *equals = memchr(line, '=', (size_t)(line_end - line));
        if (entry[0] == '[' && entry[strlen(entry) - 1] == ']') {
            copy_trimmed(section, entry + 1, entry + strlen(entry) - 1);
        } else if (entry[0] != '#' && entry[0] != ';' && equals) {
            char key[CONFIG_NAME_MAX];
            char value[CONFIG_NAME_MAX];
            copy_trimmed(key, line, equals);
            copy_trimmed(value, equals + 1, line_end);
            if (strcmp(section, "clipboard") == 0 && strcmp(key, "copy_on_select") == 0) {
                set_flag(&config->copy_on_select, value);
            } else if (strcmp(section, "clipboard") == 0 && strcmp(key, "paste_protection") == 0) {
                set_flag(&config->paste_protection, value);
            }
        }

        line = line_end + 1;
    }
}

int config_load(term_config_t *config, const char *path) {
    config_defaults(config);

    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }

    char text[CONFIG_FILE_MAX];
    size_t len = fread(text, 1, sizeof(text), file);
    fclose(file);

    config_parse(config, text, len);
    return 0;
}
