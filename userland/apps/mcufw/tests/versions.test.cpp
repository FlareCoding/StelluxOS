/* versions, run on the build machine: release parts by value, missing parts as 0, prereleases before their release,
 * build metadata ignored, and the constraints libraries state on their dependencies */

#include "versions.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

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

static bool meets(const char* version, const char* constraint) {
    bool meets_constraint = false;
    std::string error;
    return check_constraint(version, constraint, meets_constraint, error) && meets_constraint;
}

static void check_constraints() {
    check(meets("1.0.0", "") && meets("0.1", "  "), "an empty constraint meets every version");
    check(meets("1.17.4", ">=1.14.0") && !meets("1.12.0", ">=1.14.0"), "at least");
    check(meets("2.5.0", ">=2.3.4 && <3.0.0") && !meets("3.0.0", ">=2.3.4 && <3.0.0"), "a range joined by &&");
    check(meets("1.0.6", "=1.0.6") && !meets("1.0.7", "=1.0.6"), "exactly");
    check(meets("0.2", "<0.3.0") && meets("0.6.0", "<=0.6.0") && !meets("1.0", ">1.0"), "the other comparisons");

    check(meets("2.0.0", "=1.0.0 || =2.0.0") && !meets("1.5.0", "=1.0.0 || =2.0.0"), "either side of ||");
    check(meets("1.5.0", "!=1.0.0") && !meets("1.0.0", "!=1.0.0"), "! negates the term after it");
    check(meets("3.0.0", "(>=1.0.0 && <2.0.0) || =3.0.0") && !meets("2.5.0", "(>=1.0.0 && <2.0.0) || =3.0.0"),
          "parentheses group a constraint");
    check(meets("2.5.0", "=1.0.0 || >=2.0.0 && <3.0.0") && !meets("3.5.0", "=1.0.0 || >=2.0.0 && <3.0.0"),
          "&& binds before ||");
    check(meets("1.0.0", "=1.0.0)"), "a closing parenthesis left over ends the constraint, as relaxed-semver reads it");
}

static void check_compatible() {
    check(meets("1.9.9", "^1.2.0") && !meets("2.0.0", "^1.2.0") && !meets("1.1.0", "^1.2.0"),
          "^ keeps a major that is not 0, at or above the bound");
    check(meets("0.2.9", "^0.2.0") && !meets("0.3.0", "^0.2.0"), "^ keeps the minor when the major is 0");
    check(meets("0.0.3", "^0.0.3") && !meets("0.0.4", "^0.0.3"), "^ keeps the patch when major and minor are 0");
}

static void check_constraint_failures() {
    const char* unreadable[] = { "~1.0", ">= 1.0", "=1.0.a", "=01.0", ">=1.0 &&", "=1.0 | =2.0", "(=1.0", "=" };
    for (const char* constraint : unreadable) {
        bool result = false;
        std::string error;
        check(!check_constraint("1.0.0", constraint, result, error) &&
                  error == "cannot read the version constraint \"" + std::string(constraint) + "\"",
              constraint);
    }
}

static void check_semantic_versions() {
    for (const char* version : { "1", "1.2", "1.2.3", "0.0.0", "1.2.3-rc.1+b5", "2.0.16-rc.2" }) {
        check(is_semantic_version(version), version);
    }

    for (const char* version : { "", "01.2", "1.2.3.4", "1.0.a", "1.0.0-01", "1.0.0-", "1..0" }) {
        check(!is_semantic_version(version), version);
    }
}

int main() {
    check_releases();
    check_prereleases();
    check_build_metadata();
    check_constraints();

    check_compatible();
    check_constraint_failures();
    check_semantic_versions();

    if (g_failures != 0) {
        printf("versions: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("versions: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
