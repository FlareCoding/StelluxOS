/* match_query, run on the build machine: each quality in its order, the runs the palette marks, keywords, and the
 * queries that match nothing */

#include "shell/quick_match.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

struct expected_match {
    const char*   query;
    const char*   name;
    const char*   keywords;
    match_quality quality;
    const char*   runs; /* each run as start+length, space separated */
};

constexpr uint32_t MAX_REPORTS = 8;

static const expected_match MATCHES[] = {
    { "she", "Sheen", "", match_quality::prefix, "0+3" },
    { "SHE", "Sheen", "", match_quality::prefix, "0+3" },
    { "file man", "File Manager", "", match_quality::prefix, "0+8" },
    { "man", "File Manager", "", match_quality::word, "5+3" },
    { "manager", "file-manager", "", match_quality::word, "5+7" },
    { "conf", "stlxdm.conf", "", match_quality::word, "7+4" },
    { "monitor", "Displays", "monitor resolution refresh", match_quality::word, "" },
    { "res", "Displays", "monitor resolution refresh", match_quality::word, "" },
    { "ana", "File Manager", "", match_quality::substring, "6+3" },
    { "play", "Displays", "monitor", match_quality::substring, "3+4" },
    { "fmr", "File Manager", "", match_quality::letters, "0+1 5+1 11+1" },
    { "flem", "File Manager", "", match_quality::letters, "0+1 2+2 5+1" },
    { "stg", "Settings", "", match_quality::letters, "0+1 2+1 6+1" },
    { "s", "Settings", "", match_quality::prefix, "0+1" },
    { "t", "Settings", "", match_quality::substring, "2+1" },
};

static const expected_match MISSES[] = {
    { "", "Sheen", "", match_quality::none, "" },
    { "fm", "File Manager", "", match_quality::none, "" },
    { "xyz", "File Manager", "", match_quality::none, "" },
    { "sheens", "Sheen", "", match_quality::none, "" },
    { "olution", "Displays", "monitor resolution", match_quality::none, "" },
    { "zz", "", "", match_quality::none, "" },
};

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what, const expected_match& expected) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s: \"%s\" in \"%s\"\n", what, expected.query, expected.name);
    }
}

static std::string written(const query_match& match) {
    std::string out;
    for (const match_run& run : match.runs) {
        out += out.empty() ? "" : " ";
        out += std::to_string(run.start) + "+" + std::to_string(run.length);
    }

    return out;
}

int main() {
    for (const expected_match& expected : MATCHES) {
        query_match match = match_query(expected.query, expected.name, expected.keywords);
        check(match.quality == expected.quality, "matches with its quality", expected);
        check(written(match) == expected.runs, "marks its runs", expected);
    }

    for (const expected_match& expected : MISSES) {
        query_match match = match_query(expected.query, expected.name, expected.keywords);
        check(match.quality == match_quality::none && match.runs.empty(), "matches nothing", expected);
    }

    if (g_failures != 0) {
        printf("quick_match: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("quick_match: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
