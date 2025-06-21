#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stlibc/stlibc.h>

int main() {
    printf("<--- StelluxOS Init Process --->\n");
    printf("Testing complete file I/O functionality...\n\n");
    
    // Test 1: Open file for read/write with creation
    printf("1. Opening /test.txt for read/write...\n");
    int fd = open("/test.txt", O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("Failed to open /test.txt");
        printf("errno: %d (%s)\n", errno, strerror(errno));
        return 1;
    }
    printf("   Successfully opened /test.txt with fd: %d\n", fd);

    // Test 2: Write data to file
    printf("\n2. Writing data to file...\n");
    const char* message = "Hello StelluxOS File System!";
    ssize_t bytes_written = write(fd, message, strlen(message));
    if (bytes_written < 0) {
        perror("Failed to write to /test.txt");
        printf("errno: %d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("   Successfully wrote %zd bytes: '%s'\n", bytes_written, message);

    // Test 3: Seek back to beginning
    printf("\n3. Seeking back to beginning of file...\n");
    off_t pos = lseek(fd, 0, SEEK_SET);
    if (pos < 0) {
        perror("Failed to seek to beginning");
        printf("errno: %d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("   Successfully seeked to position: %ld\n", pos);

    // Test 4: Read data back from file
    printf("\n4. Reading data back from file...\n");
    char read_buffer[100];
    memset(read_buffer, 0, sizeof(read_buffer)); // Zero the buffer
    ssize_t bytes_read = read(fd, read_buffer, sizeof(read_buffer) - 1);
    if (bytes_read < 0) {
        perror("Failed to read from /test.txt");
        printf("errno: %d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    read_buffer[bytes_read] = '\0'; // Null terminate
    printf("   Successfully read %zd bytes: '%s'\n", bytes_read, read_buffer);

    // Test 5: Seek to end of file
    printf("\n5. Seeking to end of file...\n");
    pos = lseek(fd, 0, SEEK_END);
    if (pos < 0) {
        perror("Failed to seek to end");
        printf("errno: %d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("   File size: %ld bytes\n", pos);

    // Test 6: Close the file
    printf("\n6. Closing file...\n");
    int close_result = close(fd);
    if (close_result < 0) {
        perror("Failed to close /test.txt");
        printf("errno: %d (%s)\n", errno, strerror(errno));
        return 1;
    }
    printf("   Successfully closed file\n");

    printf("\n[+] All file I/O tests completed successfully!\n");
    printf("StelluxOS file system is working!\n");

    return 0;
}
