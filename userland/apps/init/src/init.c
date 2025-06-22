#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <stlibc/stlibc.h>

// Function prototypes
int launch_process(const char* process_name);
int wait_for_process(int handle, const char* process_name);

int main() {
    // Launch system processes
    int stlxdm_handle = launch_process("stlxdm");
    if (stlxdm_handle < 0) {
        printf("Failed to launch the display manager process\n");
        return -1;
    }

    // We don't need to wait for the display manager
    proc_close(stlxdm_handle);

    int shell_handle = launch_process("shell");
    if (shell_handle < 0) {
        return -1;
    }
    
    // Will automatically close the handle after the shell exits
    if (wait_for_process(shell_handle, "shell") != 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief Launches a process by name and returns its handle.
 * @param process_name The name of the process to launch (without path or extension)
 * @return Process handle on success, -1 on failure
 */
int launch_process(const char* process_name) {
    if (!process_name) {
        printf("[-] Invalid process name\n");
        return -1;
    }

    // Construct full path: /initrd/bin/<process_name>
    char full_path[256];
    int ret = snprintf(full_path, sizeof(full_path), "/initrd/bin/%s", process_name);
    if (ret < 0 || ret >= (int)sizeof(full_path)) {
        printf("[-] Process path too long for '%s'\n", process_name);
        return -1;
    }
    
    int handle = proc_create(full_path, PROC_NEW_ENV, PROC_ACCESS_ALL, PROC_HANDLE_NONE, NULL);
    if (handle < 0) {
        printf("[-] Failed to launch %s process (handle: %d)\n", process_name, handle);
        return -1;
    }

    return handle;
}

/**
 * @brief Waits for a process to complete and reports its exit status.
 * @param handle The process handle to wait for
 * @param process_name The name of the process (for logging)
 * @return 0 on success, -1 on failure
 */
int wait_for_process(int handle, const char* process_name) {
    if (handle < 0 || !process_name) {
        printf("[-] Invalid handle or process name\n");
        return -1;
    }
    
    int exit_code = 0;
    if (proc_wait(handle, &exit_code) != 0) {
        printf("[-] Failed to wait for '%s' process\n", process_name);
        return -1;
    }

    return 0;
}
