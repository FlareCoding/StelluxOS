#ifndef STLXTERM_CONFIG_H
#define STLXTERM_CONFIG_H

#include <stddef.h>

#define CONFIG_PATH "/etc/stlxterm/stlxterm.conf"

// The terminal's settings, each holding its default until the file sets it
typedef struct {
    int copy_on_select;   // [clipboard] copy_on_select: a finished selection goes to the clipboard
    int paste_protection; // [clipboard] paste_protection: lines for a program without bracketed paste ask first
} term_config_t;

void config_defaults(term_config_t *config);

// Reads INI text over the current values: keys it does not know and values other than true and false are ignored
void config_parse(term_config_t *config, const char *text, size_t len);

// Reads the file over the defaults. Returns 0, or -1 when it cannot be read, leaving the defaults.
int config_load(term_config_t *config, const char *path);

#endif // STLXTERM_CONFIG_H
