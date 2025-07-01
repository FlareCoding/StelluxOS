#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main() {
    while (1) {
        printf("$ ");
        fflush(stdout);  // Make sure prompt appears immediately

        char buffer[512] = { 0 };
        ssize_t bytes_read = read(STDIN_FILENO, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            // Null terminate the buffer
            buffer[bytes_read] = '\0';
            
            // Remove trailing newline if present
            if (bytes_read > 0 && buffer[bytes_read - 1] == '\n') {
                buffer[bytes_read - 1] = '\0';
            }
            
            // Only process non-empty commands
            if (strlen(buffer) > 0) {
                // Check for builtin commands
                if (strcmp(buffer, "clear") == 0) {
                    // Send ANSI escape sequence to clear screen and move cursor to top-left
                    printf("\033[2J\033[H");
                    fflush(stdout);
                } else if (strcmp(buffer, "exit") == 0) {
                    printf("Goodbye!\n");
                    exit(0);
                } else {
                    printf("Command '%s' not found\n", buffer);
                }
            }
        }
    }

    return 0;
}
