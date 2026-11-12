/* Lists, installs and removes the packages of the newest package release on GitHub, for this machine's
 * architecture */
#define _POSIX_C_SOURCE 200809L

#include "catalog.h"
#include "install.h"
#include "records.h"
#include "remove.h"
#include "report.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#define EVENTS_FLAG "--events"
#define RUN_NAME    "-"

typedef enum {
    OFFERED_AVAILABLE,
    OFFERED_INSTALLED,
    OFFERED_OTHER_RELEASE,
} offered_state;

static const char* const STATE_SENTENCES[] = {
    [OFFERED_AVAILABLE]     = "",
    [OFFERED_INSTALLED]     = "installed",
    [OFFERED_OTHER_RELEASE] = "installed from another release",
};

static const char* const STATE_EVENTS[] = {
    [OFFERED_AVAILABLE]     = "available",
    [OFFERED_INSTALLED]     = "installed",
    [OFFERED_OTHER_RELEASE] = "update",
};

static catalog       g_catalog;
static catalog_entry g_installed[CATALOG_ENTRIES_MAX];

static void print_usage(void) {
    printf("Usage: stlxpkg list\n"
           "       stlxpkg install <name>...\n"
           "       stlxpkg remove <name>...\n");
}

static offered_state state_of(const catalog_entry* offered) {
    catalog_entry installed;
    int found = record_read(offered->name, &installed, NULL);
    if (found != 1) {
        return OFFERED_AVAILABLE;
    }

    return strcmp(installed.sha256, offered->sha256) == 0 ? OFFERED_INSTALLED : OFFERED_OTHER_RELEASE;
}

/* Each package the release offers, marked when installed, then the installed packages it no longer offers */
static int list_packages(const char* arch) {
    printf("%s for %s:\n", g_catalog.tag, arch);
    for (size_t i = 0; i < g_catalog.count; i++) {
        const catalog_entry* offered = &g_catalog.entries[i];
        printf("  %-18s %-20s %s\n", offered->name, offered->version, STATE_SENTENCES[state_of(offered)]);
    }

    int installed_count = records_list(g_installed, CATALOG_ENTRIES_MAX);
    for (int i = 0; i < installed_count; i++) {
        if (!catalog_find(&g_catalog, g_installed[i].name)) {
            printf("  %-18s %-20s installed, not in %s\n", g_installed[i].name, g_installed[i].version, g_catalog.tag);
        }
    }

    return installed_count < 0 ? 1 : 0;
}

/* The installed packages first, read without the network, then each package the release offers with its state */
static int list_events(const char* arch) {
    int installed_count = records_list(g_installed, CATALOG_ENTRIES_MAX);
    for (int i = 0; i < installed_count; i++) {
        report_record(g_installed[i].name, g_installed[i].version);
    }

    if (installed_count < 0 || catalog_load(&g_catalog, arch) != 0) {
        report_failed(RUN_NAME);
        return 1;
    }

    for (size_t i = 0; i < g_catalog.count; i++) {
        const catalog_entry* offered = &g_catalog.entries[i];
        report_package(offered->name, offered->version, STATE_EVENTS[state_of(offered)]);
    }

    return 0;
}

/* Each package is its own transaction, and the first failure stops the run */
static int for_each_name(int count, char* args[], bool is_install) {
    for (int i = 1; i < count; i++) {
        int rc = is_install ? install_package(&g_catalog, args[i]) : remove_package(args[i]);
        if (rc != 0) {
            report_failed(args[i]);
            return 1;
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    bool has_events = argc > 1 && strcmp(argv[1], EVENTS_FLAG) == 0;
    char** args = argv + (has_events ? 2 : 1);
    int count = argc - (has_events ? 2 : 1);

    bool is_list = count == 1 && strcmp(args[0], "list") == 0;
    bool is_install = count > 1 && strcmp(args[0], "install") == 0;
    bool is_remove = count > 1 && strcmp(args[0], "remove") == 0;
    if (!is_list && !is_install && !is_remove) {
        print_usage();
        return 1;
    }

    if (has_events) {
        report_use_events(stdout);
    }

    if (records_lock() != 0) {
        report_failed(RUN_NAME);
        return 1;
    }

    if (is_remove) {
        return for_each_name(count, args, false);
    }

    struct utsname machine;
    if (uname(&machine) != 0) {
        fail(FAILURE_IO, "cannot read this machine's architecture");
        report_failed(RUN_NAME);
        return 1;
    }

    if (is_list && has_events) {
        return list_events(machine.machine);
    }

    if (catalog_load(&g_catalog, machine.machine) != 0) {
        report_failed(RUN_NAME);
        return 1;
    }

    return is_list ? list_packages(machine.machine) : for_each_name(count, args, true);
}
