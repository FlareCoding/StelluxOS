/* Lists and installs the packages of the newest package release on GitHub, for this machine's architecture */
#define _POSIX_C_SOURCE 200809L

#include "catalog.h"
#include "install.h"
#include "records.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

static catalog       g_catalog;
static catalog_entry g_installed[CATALOG_ENTRIES_MAX];

static void print_usage(void) {
    printf("Usage: stlxpkg list\n"
           "       stlxpkg install <name>...\n");
}

static void print_package(const catalog_entry* offered) {
    catalog_entry installed;
    int found = record_read(offered->name, &installed);
    const char* state = "";
    if (found == 1 && strcmp(installed.sha256, offered->sha256) == 0) {
        state = "installed";
    } else if (found == 1) {
        state = "installed from another release";
    }

    printf("  %-18s %-20s %s\n", offered->name, offered->version, state);
}

/* Each package the release offers, marked when installed, then the installed packages it no longer offers */
static int list_packages(const char* arch) {
    printf("%s for %s:\n", g_catalog.tag, arch);
    for (size_t i = 0; i < g_catalog.count; i++) {
        print_package(&g_catalog.entries[i]);
    }

    int installed_count = records_list(g_installed, CATALOG_ENTRIES_MAX);
    for (int i = 0; i < installed_count; i++) {
        if (!catalog_find(&g_catalog, g_installed[i].name)) {
            printf("  %-18s %-20s installed, not in %s\n", g_installed[i].name, g_installed[i].version, g_catalog.tag);
        }
    }

    return installed_count < 0 ? 1 : 0;
}

int main(int argc, char* argv[]) {
    bool is_list = argc == 2 && strcmp(argv[1], "list") == 0;
    bool is_install = argc > 2 && strcmp(argv[1], "install") == 0;
    if (!is_list && !is_install) {
        print_usage();
        return 1;
    }

    struct utsname machine;
    if (uname(&machine) != 0) {
        fprintf(stderr, "stlxpkg: cannot read this machine's architecture\n");
        return 1;
    }

    if (records_lock() != 0 || catalog_load(&g_catalog, machine.machine) != 0) {
        return 1;
    }

    if (is_list) {
        return list_packages(machine.machine);
    }

    for (int i = 2; i < argc; i++) {
        if (install_package(&g_catalog, argv[i]) != 0) {
            return 1;
        }
    }

    return 0;
}
