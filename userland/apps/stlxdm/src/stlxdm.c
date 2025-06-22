#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

// Define STB TrueType implementation
#define STB_TRUETYPE_IMPLEMENTATION

// Suppress warnings for STB TrueType library
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

#include "stb_truetype.h"

#pragma GCC diagnostic pop

int main() {
    printf("<--- StelluxOS Display Manager (STLXDM) --->\n");
    
    printf("Loading TTF font file into memory...\n\n");
    
    const char* font_path = "/initrd/res/fonts/UbuntuMono-Regular.ttf";
    
    // Step 1: Open the font file for reading
    printf("1. Opening font file: %s\n", font_path);
    int fd = open(font_path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open font file");
        printf("errno: %d (%s)\n", errno, strerror(errno));
        return 1;
    }
    printf("   Successfully opened font file with fd: %d\n", fd);
    
    // Step 2: Get the file size
    printf("\n2. Determining file size...\n");
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        perror("Failed to seek to end of file");
        printf("errno: %d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("   Font file size: %ld bytes\n", file_size);
    
    // Step 3: Seek back to beginning
    printf("\n3. Seeking back to beginning...\n");
    if (lseek(fd, 0, SEEK_SET) < 0) {
        perror("Failed to seek to beginning");
        printf("errno: %d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("   Successfully positioned at start of file\n");
    
    // Step 4: Allocate memory for the font data
    printf("\n4. Allocating memory for font data...\n");
    void* font_data = malloc(file_size);
    if (!font_data) {
        printf("Failed to allocate %ld bytes for font data\n", file_size);
        close(fd);
        return 1;
    }
    printf("   Successfully allocated %ld bytes at address: %p\n", file_size, font_data);
    
    // Step 5: Read the entire font file into memory
    printf("\n5. Reading font file into memory...\n");
    ssize_t total_read = 0;
    char* buffer = (char*)font_data;
    
    while (total_read < file_size) {
        ssize_t bytes_read = read(fd, buffer + total_read, file_size - total_read);
        if (bytes_read < 0) {
            perror("Failed to read from font file");
            printf("errno: %d (%s)\n", errno, strerror(errno));
            free(font_data);
            close(fd);
            return 1;
        }
        if (bytes_read == 0) {
            // EOF reached
            break;
        }
        total_read += bytes_read;
    }
    
    printf("   Successfully read %zd bytes into memory\n", total_read);
    
    // Step 6: Verify TTF magic number (basic validation)
    printf("\n6. Validating TTF file format...\n");
    if (total_read >= 4) {
        uint8_t* data = (uint8_t*)font_data;
        // TTF files start with 0x00, 0x01, 0x00, 0x00 or "OTTO" for OpenType
        if ((data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00) ||
            (data[0] == 'O' && data[1] == 'T' && data[2] == 'T' && data[3] == 'O')) {
            printf("   [+] Valid TTF/OpenType font file detected!\n");
            printf("   Magic bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n", 
                   data[0], data[1], data[2], data[3]);
        } else {
            printf("   [!] Warning: Unexpected magic bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n", 
                   data[0], data[1], data[2], data[3]);
        }
    }
    
    // Step 7: Close the file
    printf("\n7. Closing font file...\n");
    if (close(fd) < 0) {
        perror("Failed to close font file");
        printf("errno: %d (%s)\n", errno, strerror(errno));
    } else {
        printf("   Successfully closed font file\n");
    }
    
    // Summary
    printf("\n[+] Font loading completed successfully!\n");
    printf("Font: %s\n", font_path);
    printf("Size: %ld bytes\n", file_size);
    printf("Memory address: %p\n", font_data);
    printf("Ready for font rendering!\n");
    
    // Note: In a real implementation, you'd keep this font_data around
    // for rendering. For this test, we'll free it before exiting.
    printf("\nCleaning up memory...\n");
    free(font_data);
    printf("Memory freed.\n");

    return 0;
}
