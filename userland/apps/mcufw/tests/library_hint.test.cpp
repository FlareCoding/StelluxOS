/* library_hint, run on the build machine against fixtures/library_index.json gzipped: the newest release providing a
 * header, ranked for the board's architecture, and the failures when none does or the index is missing */

#include "library_hint.hpp"

#include "files.hpp"

#include <unistd.h>
#include <zlib.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

constexpr uint32_t MAX_REPORTS = 8;

constexpr const char* FIXTURE_INDEX = "fixtures/library_index.json";

// Two libraries providing one header, the second only for esp32, and two of equal rank for another
constexpr const char* RANKED_INDEX = R"({ "libraries": [
    { "name": "Zeta Same", "version": "2.0.0", "architectures": ["*"], "providesIncludes": ["Same.h", "Tie.h"] },
    { "name": "Same", "version": "1.0.0", "architectures": ["esp32"], "providesIncludes": ["Same.h"] },
    { "name": "Same", "version": "0.9.0", "architectures": ["*"], "providesIncludes": ["Old.h"] },
    { "name": "Alpha Same", "version": "1.0.0", "architectures": ["*"], "providesIncludes": ["Tie.h"] }
] })";

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const std::string& what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what.c_str());
    }
}

static void write_gzip(const std::string& path, const std::string& text) {
    gzFile file = gzopen(path.c_str(), "wb");
    gzwrite(file, text.data(), static_cast<unsigned>(text.size()));
    gzclose(file);
}

static void check_fixture(const std::string& root) {
    std::string text;
    std::string error;
    check(read_file(FIXTURE_INDEX, text, error), error);

    std::string index = join_path(root, "library_index.json.gz");
    write_gzip(index, text);

    library_suggestion suggestion;
    check(suggest_library(index, "FastLED.h", "esp32", suggestion, error) && suggestion.name == "FastLED" &&
              suggestion.version == "3.10.9",
          "the newest release providing the header is named");
    check(!suggest_library(index, "Missing.h", "esp32", suggestion, error) &&
              error == "no library in Arduino's index provides Missing.h",
          "a header no release provides fails");
    check(!suggest_library(join_path(root, "absent.json.gz"), "FastLED.h", "esp32", suggestion, error) &&
              error == "no library index at " + join_path(root, "absent.json.gz"),
          "a missing index fails");
}

static void check_ranking(const std::string& root) {
    std::string index = join_path(root, "ranked.json.gz");
    write_gzip(index, RANKED_INDEX);

    library_suggestion suggestion;
    std::string error;
    check(suggest_library(index, "Same.h", "esp32", suggestion, error) && suggestion.name == "Same",
          "a release for the board's architecture, named for the header, wins");
    check(suggest_library(index, "Same.h", "avr", suggestion, error) && suggestion.name == "Zeta Same",
          "on another architecture the release for every architecture wins");
    check(!suggest_library(index, "Old.h", "esp32", suggestion, error), "only a library's newest release counts");
    check(suggest_library(index, "Tie.h", "esp32", suggestion, error) && suggestion.name == "Alpha Same",
          "equal ranks go to the first name");
}

int main() {
    char made[] = "/tmp/mcufw-library-hint-XXXXXX";
    char resolved[PATH_MAX];
    if (!mkdtemp(made) || !realpath(made, resolved)) {
        printf("library_hint: cannot make a temporary folder\n");
        return 1;
    }

    std::string root = resolved;
    check_fixture(root);
    check_ranking(root);
    std::string remove = "rm -rf " + root;
    check(system(remove.c_str()) == 0, "the temporary folder is removed");

    if (g_failures != 0) {
        printf("library_hint: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("library_hint: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
