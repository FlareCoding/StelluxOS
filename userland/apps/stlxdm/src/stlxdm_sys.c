#define _POSIX_C_SOURCE 199309L
#include "stlxdm_sys.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stlibc/stlibc.h>

/**
 * @brief Launches the stlxterm terminal application
 */
void stlxdm_launch_terminal() {
    const char* process_name = "stlxterm";
    
    // Construct full path: /initrd/bin/stlxterm
    char full_path[256];
    int ret = snprintf(full_path, sizeof(full_path), "/initrd/bin/%s", process_name);
    if (ret < 0 || ret >= (int)sizeof(full_path)) {
        printf("[STLXDM_SYS] Process path too long for '%s'\n", process_name);
        return;
    }
    
    int handle = stlx_proc_create(full_path, PROC_NEW_ENV, PROC_ACCESS_ALL, PROC_HANDLE_NONE, NULL);
    if (handle < 0) {
        printf("[STLXDM_SYS] Failed to launch %s process (handle: %d)\n", process_name, handle);
        return;
    }

    stlx_proc_close(handle);
}

/**
 * @brief Launches a process by name and returns its handle
 * @param process_name The name of the process to launch (without path or extension)
 * @return Process handle on success, -1 on failure
 */
int stlxdm_launch_process(const char* process_name) {
    if (!process_name) {
        printf("[STLXDM_SYS] Invalid process name\n");
        return -1;
    }

    // Construct full path: /initrd/bin/<process_name>
    char full_path[256];
    int ret = snprintf(full_path, sizeof(full_path), "/initrd/bin/%s", process_name);
    if (ret < 0 || ret >= (int)sizeof(full_path)) {
        printf("[STLXDM_SYS] Process path too long for '%s'\n", process_name);
        return -1;
    }
    
    int handle = stlx_proc_create(full_path, PROC_NEW_ENV, PROC_ACCESS_ALL, PROC_HANDLE_NONE, NULL);
    if (handle < 0) {
        printf("[STLXDM_SYS] Failed to launch %s process (handle: %d)\n", process_name, handle);
        return -1;
    }

    return handle;
}
