/* has_memory_for, run on the build machine with free memory it is told: room past the reserve, too little, the
 * reserve itself, and a kernel that cannot say */

#include "free_memory.hpp"

#include <cstdint>
#include <cstdio>

constexpr uint64_t MB = 1ull << 20;

static uint64_t g_checks;
static uint64_t g_failures;
static uint64_t g_free_bytes;

static uint64_t told_free_memory() {
    return g_free_bytes;
}

static void check(bool holds, const char* what) {
    g_checks++;
    if (!holds) {
        g_failures++;
        printf("free_memory: failed: %s\n", what);
    }
}

int main() {
    g_free_bytes = 64 * MB;
    check(has_memory_for(32 * MB, told_free_memory), "32 MB fit in 64 MB free with the reserve to spare");
    check(has_memory_for(48 * MB, told_free_memory), "48 MB fit exactly up to the 16 MB reserve");
    check(!has_memory_for(48 * MB + 1, told_free_memory), "a byte past the reserve does not fit");

    g_free_bytes = 3 * MB;
    check(!has_memory_for(1, told_free_memory), "nothing fits once free memory is under the reserve");

    g_free_bytes = UINT64_MAX;
    check(has_memory_for(1024 * MB, told_free_memory), "a kernel that cannot say leaves every allocation to try");

    if (g_failures != 0) {
        printf("free_memory: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("free_memory: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
