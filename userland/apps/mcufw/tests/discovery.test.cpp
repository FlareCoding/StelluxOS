/* discovery, run on the build machine: GCC's messages read for the missing header, and library discovery driven by a
 * stand-in preprocessor, alone and probing ahead, until it finishes or stops */

#include "discovery.hpp"

#include "files.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;
constexpr size_t AHEAD_SLOTS = 3;
constexpr size_t TURNS_MAX = 100;  // far past any case here, so discovery that loops fails a check

// GCC 14's message for a missing header, and the form older compilers wrote without the source line
constexpr const char* GCC_MISSING =
    "/b/sketch/Demo.ino.cpp.merged:3:10: fatal error: Display.h: No such file or directory\n"
    "    3 | #include <Display.h>\n"
    "      |          ^~~~~~~~~~~\n"
    "compilation terminated.\n";
constexpr const char* OLD_GCC_MISSING = "/b/x.cpp:1:21: fatal error: sub/Bus.h: No such file or directory\n";

struct stand_in {
    std::string source;     // the file a probe preprocesses
    std::string header;     // the header it includes, empty for none
    std::string providing;  // the include folder that provides it
};

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

static void write_tree(const std::string& root, const std::vector<std::string>& files) {
    for (const std::string& file : files) {
        std::string path = join_path(root, file);
        std::string error;
        bool changed = false;
        check(make_directories(parent_directory(path), error) && write_file_if_changed(path, "\n", changed, error),
              "wrote " + path);
    }
}

static void check_reading() {
    check(read_probe_result(0, "").status == probe_status::complete, "success completes");

    probe_result missing = read_probe_result(1, GCC_MISSING);
    check(missing.status == probe_status::missing && missing.header == "Display.h" && missing.output == GCC_MISSING,
          "GCC's quoted include line names the header");

    probe_result coloured = read_probe_result(1, "\x1b[01m\x1b[K    3 | #include \"Bus.h\"\x1b[m\n");
    check(coloured.status == probe_status::missing && coloured.header == "Bus.h", "colours are left out");

    probe_result old = read_probe_result(1, OLD_GCC_MISSING);
    check(old.status == probe_status::missing && old.header == "sub/Bus.h", "a fatal error line is the fallback");

    check(read_probe_result(1, "x.cpp:2:1: error: expected ';'\n").status == probe_status::complete,
          "a failure naming no header completes, as arduino-cli reads it");
    check(read_probe_result(1, "").status == probe_status::failed, "a failure printing nothing fails");

    probe_result later = read_probe_result(1, "a.cpp:1:10: note: here\n    1 | #include <Lib.h>\n");
    check(later.status == probe_status::missing && later.header == "Lib.h",
          "an include line below the first line of output is found, as the detector's regex reads each line");
}

static library made_library(const std::string& directory, const std::string& name, library_layout layout,
                            const std::vector<std::string>& headers) {
    library made = {};
    made.name = name;
    made.version = "1.0";
    made.directory = directory;
    made.folder_name = file_name(directory);

    made.location = library_location::installed;
    made.layout = layout;
    made.architectures = { "*" };
    made.headers = headers;
    if (layout == library_layout::flat && is_directory(join_path(directory, "utility"))) {
        made.utility_directory = join_path(directory, "utility");
    }

    return made;
}

static property_table probe_properties() {
    property_table properties;
    properties.set("compiler.cpp.flags", "-MMD -c");
    properties.set("recipe.cpp.o.pattern",
                   "\"g++\" {compiler.cpp.flags} {includes} \"{source_file}\" -o \"{object_file}\"");
    return properties;
}

// What the stand-in preprocessor answers a probe: the header its source includes, missing until a folder providing it
// is among the probe's `-I` words
static probe_result answer(const probe& asked, const std::vector<stand_in>& sources) {
    for (const stand_in& known : sources) {
        if (known.source != asked.source || known.header.empty()) {
            continue;
        }

        bool provided = std::find(asked.words.begin(), asked.words.end(), "-I" + known.providing) != asked.words.end();
        if (!provided) {
            std::string output = asked.source + ":1:10: fatal error: " + known.header + ": No such file or directory\n";
            return read_probe_result(1, output);
        }
    }

    return read_probe_result(0, "");
}

// Runs discovery as the build does, with probes ahead answered when they were offered, and gives how it ended
static bool run_discovery(library_discovery& discovery, const std::vector<stand_in>& sources, bool probes_ahead,
                          discovery_failure& failure, uint64_t& hits) {
    std::map<std::vector<std::string>, probe_result> ahead;
    bool offering = false;
    size_t turns = 0;
    hits = 0;
    while (const probe* turn = discovery.turn_probe()) {
        if (++turns > TURNS_MAX) {
            check(false, "discovery ends within its turn cap");
            return false;
        }

        auto found = ahead.find(turn->words);
        bool runs_alone = found == ahead.end();
        probe_result result = runs_alone ? answer(*turn, sources) : found->second;
        hits += runs_alone ? 0 : 1;
        if (runs_alone) {
            ahead.clear();
        }

        if (!discovery.take_turn_result(result, failure)) {
            return false;
        }

        bool completed = result.status == probe_status::complete;
        offering = probes_ahead && (offering || (runs_alone && completed));
        if (!offering || !completed) {
            continue;
        }

        for (const probe& later : discovery.probes_ahead(AHEAD_SLOTS)) {
            ahead[later.words] = answer(later, sources);
        }
    }

    return true;
}

static bool same_turns(const std::vector<reached_probe>& a, const std::vector<reached_probe>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const reached_probe& x, const reached_probe& y) {
        return x.turn.words == y.turn.words && x.result.status == y.result.status && x.result.header == y.result.header;
    });
}

static void check_discovery(const std::string& root) {
    std::string build = join_path(root, "build");
    std::string display = join_path(root, "libs/Display");
    std::string bus = join_path(root, "libs/Bus");
    write_tree(root, { "libs/Display/src/Display.h", "libs/Display/src/Display.cpp", "libs/Bus/Bus.h",
                       "libs/Bus/Bus.cpp", "libs/Bus/utility/pins.c" });

    std::vector<library> libraries = { made_library(display, "Display", library_layout::recursive, { "Display.h" }),
                                       made_library(bus, "Bus", library_layout::flat, { "Bus.h" }) };
    std::string merged = join_path(build, "sketch/Demo.ino.cpp.merged");
    std::string extra = join_path(build, "sketch/extra.cpp");
    std::vector<stand_in> sources = {
        { merged, "Display.h", join_path(display, "src") },
        { join_path(display, "src/Display.cpp"), "Bus.h", bus },
        { join_path(bus, "utility/pins.c"), "", "" },
    };

    std::vector<std::string> start = { "/core", "/variant" };
    library_discovery alone(probe_properties(), libraries, "esp32", build, { merged, extra }, start);
    discovery_failure failure;
    uint64_t hits = 0;
    check(run_discovery(alone, sources, false, failure, hits), "discovery finishes alone");
    check(alone.libraries_added() == std::vector<const library*>({ &libraries[0], &libraries[1] }),
          "a header adds its library, whose own header adds the next");

    std::vector<std::string> included = { "/core", "/variant", join_path(display, "src"), bus };
    check(alone.include_directories() == included, "each library's source folder joins the include path");
    check(alone.files_done() == 5 && alone.files_queued() == 5, "every queued file completed");

    const std::vector<reached_probe>& reached = alone.reached();
    check(reached.size() == 7 && reached[0].result.status == probe_status::missing &&
              reached[1].turn.source == merged && reached[1].result.status == probe_status::complete,
          "a file is probed again once its library joined");

    bool depfiles_found = std::any_of(reached.begin(), reached.end(), [&](const reached_probe& item) {
        return item.turn.depfile == join_path(build, "libraries/Bus/utility/pins.c.libsdetect.d");
    });
    check(depfiles_found && reached[0].turn.depfile == merged + ".libsdetect.d", "depfiles go under the build tree");

    const reached_probe& pins = reached.back();
    check(std::find(pins.turn.words.begin(), pins.turn.words.end(), "-I" + join_path(bus, "utility")) !=
              pins.turn.words.end(),
          "a flat library's own files also see its utility/ folder");

    library_discovery ahead(probe_properties(), libraries, "esp32", build, { merged, extra }, start);
    check(run_discovery(ahead, sources, true, failure, hits) && hits > 0, "discovery finishes probing ahead");
    check(ahead.libraries_added() == alone.libraries_added() &&
              ahead.include_directories() == alone.include_directories(),
          "probing ahead adds what probing alone adds");
    check(same_turns(ahead.reached(), alone.reached()), "probing ahead gives every turn's probe and result alone");

    library_discovery offering(probe_properties(), libraries, "esp32", build, { merged, extra }, start);
    std::vector<probe> first = offering.probes_ahead(AHEAD_SLOTS);
    check(!first.empty() && first[0].source == merged && offering.probes_ahead(AHEAD_SLOTS).empty(),
          "probes ahead start at the turn's file, and each probe's words are offered once");
}

static void check_sources_and_errors(const std::string& root) {
    std::string tree = join_path(root, "tree/sketch");
    write_tree(root, { "tree/sketch/Demo.ino.cpp", "tree/sketch/Demo.ino.cpp.merged", "tree/sketch/extra.cpp",
                       "tree/sketch/src/a/b.cpp" });

    std::vector<std::string> sources;
    std::string error;
    std::vector<std::string> expected = { join_path(tree, "Demo.ino.cpp.merged"), join_path(tree, "extra.cpp"),
                                          join_path(tree, "src/a/b.cpp") };
    check(discovery_sources(tree, "Demo.ino", sources, error) && sources == expected,
          "discovery probes the merged sketch, the tree's files but the .ino.cpp, then src/");

    property_table broken = probe_properties();
    broken.set("recipe.cpp.o.pattern", "g++ {includes} \"{source_file}");
    std::vector<library> none;
    library_discovery unsplit(broken, none, "esp32", join_path(root, "build"), { expected[0] }, { "/core" });
    check(unsplit.turn_probe() && !unsplit.turn_probe()->error.empty(), "a recipe that cannot split gives its error");
}

static void check_stops(const std::string& root) {
    std::string build = join_path(root, "build");
    std::string display = join_path(root, "libs/Display");
    std::string merged = join_path(build, "sketch/Demo.ino.cpp.merged");
    std::vector<library> libraries = { made_library(display, "Display", library_layout::recursive, { "Display.h" }) };

    std::vector<std::string> start = { "/core" };
    discovery_failure failure;
    uint64_t hits = 0;
    std::vector<stand_in> unknown = { { merged, "Nothing.h", "/nowhere" } };
    library_discovery unresolved(probe_properties(), libraries, "esp32", build, { merged }, start);
    check(!run_discovery(unresolved, unknown, false, failure, hits) && failure.stop == discovery_stop::unresolved &&
              failure.header == "Nothing.h" && !failure.output.empty(),
          "a header no library provides stops discovery");

    std::vector<stand_in> again = { { merged, "Display.h", join_path(display, "src") },
                                    { join_path(display, "src/Display.cpp"), "Display.h", "/nowhere" } };
    library_discovery added(probe_properties(), libraries, "esp32", build, { merged }, start);
    check(!run_discovery(added, again, false, failure, hits) && failure.stop == discovery_stop::already_added &&
              failure.library == "Display",
          "a header a library already added should provide stops it");

    libraries[0].refusal = "is precompiled, which mcufw does not link";
    std::vector<stand_in> wanted = { { merged, "Display.h", join_path(display, "src") } };
    library_discovery refused(probe_properties(), libraries, "esp32", build, { merged }, start);
    check(!run_discovery(refused, wanted, false, failure, hits) && failure.stop == discovery_stop::refused &&
              failure.library == "Display is precompiled, which mcufw does not link",
          "a refused library stops discovery only once it would join");

    library_discovery failed(probe_properties(), libraries, "esp32", build, { merged }, start);
    check(!failed.take_turn_result(read_probe_result(1, ""), failure) && failure.stop == discovery_stop::failed,
          "a preprocessor printing nothing stops it");

    libraries[0].refusal.clear();
    std::string locked = join_path(display, "src");
    bool unlisted = chmod(locked.c_str(), 0) == 0;
    library_discovery unreadable(probe_properties(), libraries, "esp32", build, { merged }, start);
    check(unlisted && !run_discovery(unreadable, wanted, false, failure, hits) &&
              failure.stop == discovery_stop::unreadable && failure.library == "Display" &&
              failure.output.find(locked) != std::string::npos,
          "a library whose files cannot be listed stops discovery naming the folder");
    chmod(locked.c_str(), 0755);
}

int main() {
    char made[] = "/tmp/mcufw-discovery-XXXXXX";
    char resolved[PATH_MAX];
    if (!mkdtemp(made) || !realpath(made, resolved)) {
        printf("discovery: cannot make a temporary folder\n");
        return 1;
    }

    std::string root = resolved;
    check_reading();
    check_discovery(root);
    check_stops(root);
    check_sources_and_errors(root);

    std::string remove = "rm -rf " + root;
    check(system(remove.c_str()) == 0, "the temporary folder is removed");

    if (g_failures != 0) {
        printf("discovery: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("discovery: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
