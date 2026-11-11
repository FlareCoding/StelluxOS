#ifndef STLXCONF_CONF_H
#define STLXCONF_CONF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STLXCONF_PATH          "/etc/stlxdm/stlxdm.conf"
#define STLXCONF_DROP_IN_DIR   "/etc/stlxdm/conf.d"
#define STLXCONF_DEFAULT_ICON  "/etc/res/icons/icon_unknown_32x32.bmp"
#define STLXCONF_MAX_TASKBAR   16
#define STLXCONF_MAX_SHORTCUTS 32
#define STLXCONF_MAX_AUTOSTART 8
#define STLXCONF_MAX_URL_HANDLERS 8
#define STLXCONF_MAX_FILE_HANDLERS 32
#define STLXCONF_MAX_ACTIONS 32

#define STLXCONF_DEFAULT_KEY_REPEAT_DELAY_MS    400
#define STLXCONF_DEFAULT_KEY_REPEAT_INTERVAL_MS 40

/* Which compositor composes the desktop: auto the GPU when stlxdm can make
 * its compositor, cpu the CPU */
typedef enum {
    STLXCONF_COMPOSITOR_AUTO,
    STLXCONF_COMPOSITOR_CPU,
} stlxconf_compositor_t;

/* For tests, a failure the next GPU compose makes after a reload changes
 * this from none: a read of an address no context maps, or a spin past its
 * deadline */
typedef enum {
    STLXCONF_COMPOSE_FAULT_NONE,
    STLXCONF_COMPOSE_FAULT_FAULT,
    STLXCONF_COMPOSE_FAULT_HANG,
} stlxconf_compose_fault_t;

/* How much frosted wallpaper a glass surface shows, none at solid */
typedef enum {
    STLXCONF_GLASS_LEVEL_SOLID,
    STLXCONF_GLASS_LEVEL_SUBTLE,
    STLXCONF_GLASS_LEVEL_GLASS,
    STLXCONF_GLASS_LEVEL_CLEAR,
} stlxconf_glass_level_t;

/* The tint of glass surfaces, a warm dark brown or a neutral dark gray */
typedef enum {
    STLXCONF_GLASS_TINT_WARM,
    STLXCONF_GLASS_TINT_NEUTRAL,
} stlxconf_glass_tint_t;

/* One pinned launcher in the dock, order preserved from the file, args space
 * separated. The name is the section suffix, kept for faithful re-serialization. */
typedef struct {
    char name[64];
    char label[64];
    char path[256];
    char args[256];
    char icon_path[256];
    bool from_drop_in; /* owned by a drop-in file, never written back */
} stlxconf_pin_t;

/* One global key binding. The key string is a plus-separated chord
 * such as ctrl+alt+t, exec is the only action, and args are space
 * separated. */
typedef struct {
    char name[64];
    char key[64];
    char action[32];
    char path[256];
    char args[256];
    bool from_drop_in;
} stlxconf_shortcut_t;

/* One program spawned at startup, args space separated */
typedef struct {
    char name[64];
    char path[256];
    char args[256];
    bool from_drop_in;
} stlxconf_autostart_t;

/* The program that opens URLs of one scheme, the section suffix. args are
 * space separated, and the URL follows them as the last argument. */
typedef struct {
    char scheme[32];
    char path[256];
    char args[256];
    bool from_drop_in;
} stlxconf_url_handler_t;

/* The program that opens files of one MIME type, the section suffix such as text/plain. args are space separated,
 * and the file's path follows them as the last argument. */
typedef struct {
    char mime_type[128];
    char path[256];
    char args[256];
    bool from_drop_in;
} stlxconf_file_handler_t;

/* One entry Quick Actions lists beside the apps, the name being the section suffix. `keywords` are space separated
 * words that also find it, and `action` is exec, which runs `path` with its space separated `args`. */
typedef struct {
    char name[64];
    char label[64];
    char subtitle[128]; /* what it does, shown under the label */
    char keywords[128];
    char icon_path[256];
    char action[32];
    char path[256];
    char args[256];
    bool from_drop_in;
} stlxconf_action_t;

/**
 * The desktop configuration, parsed from an INI-style file. Every
 * field holds its default until the file overrides it, so a missing
 * or partial file always yields a usable desktop.
 */
typedef struct {
    /* [desktop] wallpaper is scaled to cover the screen, an empty
     * path falls back to the flat bg_color */
    uint32_t bg_color;
    char     wallpaper[256];

    /* [desktop] the compositor, and for tests each GPU frame also composed
     * on the CPU and compared, and a failure made in a GPU compose */
    stlxconf_compositor_t    compositor;
    bool                     compare_frames;
    stlxconf_compose_fault_t compose_fault;

    /* [theme] bar colors, the bar text size, and the glass level and tint */
    uint32_t bar_color;
    uint32_t bar_font_size;
    uint32_t text_color;
    stlxconf_glass_level_t glass_level;
    stlxconf_glass_tint_t  glass_tint;

    /* [input] keyboard repeat, a delay of 0 disables it */
    uint32_t key_repeat_delay_ms;
    uint32_t key_repeat_interval_ms;

    stlxconf_pin_t pins[STLXCONF_MAX_TASKBAR];
    uint32_t pin_count;

    stlxconf_shortcut_t shortcuts[STLXCONF_MAX_SHORTCUTS];
    uint32_t shortcut_count;

    stlxconf_autostart_t autostart[STLXCONF_MAX_AUTOSTART];
    uint32_t autostart_count;

    stlxconf_url_handler_t url_handlers[STLXCONF_MAX_URL_HANDLERS];
    uint32_t url_handler_count;

    stlxconf_file_handler_t file_handlers[STLXCONF_MAX_FILE_HANDLERS];
    uint32_t file_handler_count;

    stlxconf_action_t actions[STLXCONF_MAX_ACTIONS];
    uint32_t action_count;
} stlxconf_t;

/**
 * @brief Resets every field to the built-in defaults.
 */
void stlxconf_defaults(stlxconf_t* conf);

/**
 * @brief Loads the configuration file over the defaults.
 * @return 0 on success, -1 when the file cannot be read (the config
 *         holds the defaults either way).
 */
int stlxconf_load(stlxconf_t* conf, const char* path);

/**
 * @brief Adds the launchers, shortcuts, autostart entries, URL handlers, file
 * handlers and actions of every .conf file in `dir`, in name order, to the
 * loaded configuration.
 * They stay out of stlxconf_save, since their files own them, and any other
 * section of a drop-in file is ignored.
 * @return 0 on success, also when the directory does not exist, -1 when
 *         it or one of its files cannot be read.
 */
int stlxconf_load_drop_ins(stlxconf_t* conf, const char* dir);

/**
 * @brief Writes the configuration back as canonical INI text.
 *
 * Sections come out grouped in a fixed order, so a load of the
 * written file always reproduces the same configuration.
 * @return 0 on success, -1 when the file cannot be written.
 */
int stlxconf_save(const stlxconf_t* conf, const char* path);

/**
 * @brief Names a glass level as the file writes it.
 * @return the `glass_level` value for `level`, such as "solid".
 */
const char* stlxconf_glass_level_name(stlxconf_glass_level_t level);

/**
 * @brief Names a glass tint as the file writes it.
 * @return the `glass_tint` value for `tint`, such as "warm".
 */
const char* stlxconf_glass_tint_name(stlxconf_glass_tint_t tint);

/**
 * @brief Finds the handler for URLs of `scheme`, compared without case.
 *
 * The main file's handlers come before the drop-in files', so a handler
 * the user configured wins over one a package installed.
 * @return the first handler for the scheme, NULL when none is configured.
 */
const stlxconf_url_handler_t* stlxconf_find_url_handler(const stlxconf_t* conf,
                                                        const char* scheme);

/**
 * @brief Finds the handler for files of `mime_type`, compared without case.
 *
 * The main file's handlers come before the drop-in files', so a handler
 * the user configured wins over one a package installed.
 * @return the first handler for the type, NULL when none is configured.
 */
const stlxconf_file_handler_t* stlxconf_find_file_handler(const stlxconf_t* conf,
                                                          const char* mime_type);

/**
 * @brief Splits a space-separated args value into arguments, in place.
 *
 * Each separator in `args` becomes a terminator, and `argv` points into
 * `args`. At most `capacity - 1` arguments are stored, followed by NULL,
 * so `capacity` must be at least 1.
 * @return the number of arguments stored.
 */
int stlxconf_split_args(char* args, const char* argv[], int capacity);

#ifdef __cplusplus
}
#endif

#endif /* STLXCONF_CONF_H */
