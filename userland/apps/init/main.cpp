// #include <sched/sched.h>
// #include <time/time.h>
// #include <dynpriv/dynpriv.h>
// #include <ipc/shm.h>
// #include <core/klog.h>

#include <stlibc/stlibc.h>
#include <stlibc/memory/malloc.h>
#include <stlibc/system/syscall.h>
#include <stlibc/memory/memory.h>
#include <stlibc/string/string.h>
#include <stlibc/string/format.h>

// Helper function to make syscalls cleaner
void write_str(const char* str) {
    size_t len = strlen(str);
    syscall(SYS_WRITE, 0, reinterpret_cast<uint64_t>(str), len, 0, 0, 0);
}

// Test basic string operations
void test_basic_string_ops() {
    char buffer[256];
    char str1[] = "Hello";
    char str2[] = "World";
    
    // Test strcpy and strcat
    strcpy(buffer, str1);
    strcat(buffer, " ");
    strcat(buffer, str2);
    printf("Basic string concatenation: %s\n", buffer);

    // Test strncpy with padding
    char padded[16] = "XXXXXXXXXXXXXXX";
    strncpy(padded, "Hello", 5);
    printf("strncpy with padding: %s\n", padded);

    // Test string comparison
    if (strcmp(str1, "Hello") == 0) {
        printf("String comparison works!\n");
    }
}

// Test string search functions
void test_string_search() {
    const char* text = "Hello, this is a test string";
    char buffer[256];

    // Test strchr
    char* comma = strchr(text, ',');
    if (comma) {
        strncpy(buffer, comma + 2, 4);
        buffer[4] = '\0';
        printf("Found after comma: %s\n", buffer);
    }

    // Test strstr
    const char* test = strstr(text, "test");
    if (test) {
        strncpy(buffer, test, 4);
        buffer[4] = '\0';
        printf("Found substring: %s\n", buffer);
    }
}

// Test string formatting
void test_string_formatting() {
    // Test basic integer formatting
    printf("Integer: %d, Hex: %x, Octal: %o\n", 42, 42, 42);

    // Test string formatting with width and precision
    printf("String: %10s, Truncated: %.5s\n", "Hello World", "Hello World");

    // Test flags and padding
    printf("Padded: %05d, Left: %-5d, Signed: %+d\n", 42, 42, 42);

    // Test hex formatting with prefix
    printf("Hex with prefix: %#x, Uppercase: %X\n", 42, 42);

    // Test multiple arguments
    printf("Multiple: %s %d %c\n", "Test", 123, 'A');
}

// Test complex formatting scenarios
void test_complex_formatting() {
    // Test width and precision with strings
    printf("Width and precision test:\n");
    printf("%-10.5s|%10.5s\n", "Hello World", "Hello World");

    // Test number formatting with different bases
    printf("Decimal: %d\n"
           "Hex: %#x\n"
           "Octal: %#o\n"
           "Unsigned: %u\n",
           42, 42, 42, 42);

    // Test character formatting
    printf("Char: %c, Padded: %5c\n", 'A', 'B');

    // Test special cases
    printf("%4d %-4s\n", 100, "KB");
    printf("%4d %-4s\n", 42, "B");
    printf("%4d %-4s\n", 1004, "KB");
    printf("%4d %-4s\n", 472, "MB");
}

extern "C" int main() {
    printf("Starting string manipulation and formatting tests...\n\n");

    printf("=== Basic String Operations ===\n");
    test_basic_string_ops();
    printf("\n");

    printf("=== String Search Functions ===\n");
    test_string_search();
    printf("\n");

    printf("=== Basic String Formatting ===\n");
    test_string_formatting();
    printf("\n");

    printf("=== Complex Formatting Scenarios ===\n");
    test_complex_formatting();
    printf("\n");

    printf("All string tests completed\n");
    return 0;
}
