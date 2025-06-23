#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

#define _POSIX_C_SOURCE 199309L
#include <time.h>

#include <stlibc/stlibc.h>

int main() {
    // printf("[SHELL] Starting Unix Domain Socket client test...\n");
    
    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd < 0) {
        printf("[SHELL] ERROR: socket() failed: %d\n", errno);
        return 1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/stlxdm.socket");
    
    if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[SHELL] ERROR: connect() failed: %d\n", errno);
        close(client_fd);
        return 1;
    }
    
    const char* message = "hello unix socket";
    ssize_t bytes = write(client_fd, message, strlen(message));
    
    if (bytes < 0) {
        printf("[SHELL] ERROR: write() failed: %d\n", errno);
    }
    
    close(client_fd);
    printf("[SHELL] Unix Domain Socket client test completed!\n");
    
    return 0;
}
