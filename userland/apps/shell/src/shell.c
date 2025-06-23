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
    struct timespec ts = { 5, 0 }; // 5 seconds
    nanosleep(&ts, NULL);
    printf("[SHELL] Starting...\n");
    
    // === SHARED MEMORY PING-PONG TEST ===
    printf("[SHELL] Starting shared memory ping-pong test...\n");
    
    // Open the shared memory region created by stlxdm
    shm_handle_t shm_handle = stlx_shm_open("ping_pong_test");
    if (shm_handle == 0) {
        printf("[SHELL] ERROR: Failed to open shared memory region\n");
        return 1;
    }
    printf("[SHELL] Opened shared memory region with handle: %lu\n", shm_handle);
    
    // Map the shared memory
    void* shm_ptr = stlx_shm_map(shm_handle, SHM_MAP_READ | SHM_MAP_WRITE);
    if (shm_ptr == NULL) {
        printf("[SHELL] ERROR: Failed to map shared memory\n");
        return 1;
    }
    printf("[SHELL] Mapped shared memory at address: 0x%lx\n", (uint64_t)shm_ptr);
    
    // Start the ping-pong test
    uint32_t* counter = (uint32_t*)shm_ptr;
    uint32_t last_value = 0;
    int rounds = 0;
    const int max_rounds = 10;
    
    printf("[SHELL] Starting ping-pong with initial value: %u\n", *counter);
    
    while (rounds < max_rounds) {
        // Check if stlxdm initialized or updated the value
        if (*counter != last_value) {
            printf("[SHELL] Received: %u\n", *counter);
            last_value = *counter;
            
            // Increment and write back
            *counter = *counter + 1;
            printf("[SHELL] Sent: %u\n", *counter);
            last_value = *counter;
            rounds++;
            
            // Small delay to make it easier to follow
            struct timespec ts = { 0, 100 * 1000 * 1000 }; // 100ms
            nanosleep(&ts, NULL);
        } else {
            // Small delay while waiting
            struct timespec ts = { 0, 10 * 1000 * 1000 }; // 10ms
            nanosleep(&ts, NULL);
        }
    }
    
    printf("[SHELL] Ping-pong test completed after %d rounds. Final value: %u\n", rounds, *counter);
    
    // Clean up shared memory
    if (stlx_shm_unmap(shm_handle, shm_ptr) != 0) {
        printf("[SHELL] ERROR: Failed to unmap shared memory\n");
    }
    
    printf("[SHELL] Shared memory ping-pong test completed!\n");

    struct timespec ts2 = { 2, 0 }; // 2 seconds
    nanosleep(&ts2, NULL);

    printf("[SHELL] Starting Unix Domain Socket client test...\n");

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
