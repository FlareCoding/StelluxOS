/* versions, run on the build machine: release parts by value, missing parts as 0, prereleases before their release,
 * and build metadata ignored */

#include "versions.hpp"

#include <cstdint>
#include <cstdio>

constexpr uint32_t MAX_REPORTS = 8;

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static bool is_older(const char* a, const char* b) {
    return compare_versions(a, b) < 0 && compare_versions(b, a) > 0;
}

static bool is_same(const char* a, const char* b) {
    return compare_versions(a, b) == 0 && compare_versions(b, a) == 0;
}

static void check_releases() {
    check(is_older("3.3.2", "3.3.12"), "parts compare as numbers");
    check(is_older("2411", "2601"), "a single number is a version");
    check(is_same("1.0", "1.0.0"), "a missing part counts as 0");
    check(is_same("007", "7"), "leading zeros do not count");
    check(is_older("1.0.9", "1.1"), "an earlier part decides");

    check(is_same("2.0.0", "2.0.0"), "a version equals itself");
    check(is_older("1.0.1", "1.0.a"), "a number part comes before a text part");
    check(is_older("1.0.a", "1.0.b"), "text parts compare as text");
}

static void check_prereleases() {
    check(is_older("1.0.0-rc1", "1.0.0"), "a prerelease comes before its release");
    check(is_older("1.0.0-rc.2", "1.0.0-rc.10"), "numeric prerelease parts compare as numbers");
    check(is_older("1.0.0-1", "1.0.0-alpha"), "a number comes before text");
    check(is_older("1.0.0-alpha", "1.0.0-alpha.1"), "more prerelease parts come later");
    check(is_older("1.0.0-beta", "1.0.1-alpha"), "the release decides before the prerelease");
}

static void check_build_metadata() {
    check(is_same("1.0.0+build5", "1.0.0"), "build metadata does not count");
    check(is_same("1.0.0-rc1+a", "1.0.0-rc1+b"), "build metadata does not count between prereleases");
}

int main() {
    check_releases();
    check_prereleases();
    check_build_metadata();

    if (g_failures != 0) {
        printf("versions: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("versions: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
