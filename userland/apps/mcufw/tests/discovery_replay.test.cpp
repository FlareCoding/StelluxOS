/* discovery_replay, run on the build machine in a temporary build tree: a recorded discovery answered turn by turn,
 * each change that stops it, among them arduino-cli's .libsdetect.d rule, and rebuilds probing ahead as 4 jobs do */

#include "discovery_replay.hpp"

#include "files.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS   = 8;
constexpr int64_t  NS_PER_SECOND = 1000000000;
constexpr int64_t  PAST_NS       = 1700000000 * NS_PER_SECOND;
constexpr int64_t  PROBED_NS     = PAST_NS + 10 * NS_PER_SECOND;
constexpr int64_t  EDITED_NS     = PAST_NS + 20 * NS_PER_SECOND;

// The free slots beside the turn's probe with 4 jobs
constexpr size_t AHEAD_SLOTS = 3;

// A file the stand-in preprocessor knows: the header it includes, if any, and the folder that provides it
struct stand_in {
    std::string source;
    std::string header;
    std::string providing;
};

static uint64_t    g_checks;
static uint64_t    g_failures;
static std::string g_root;

static const std::vector<std::string> CORE_ONLY = { "core" };
static const std::vector<std::string> WITH_FASTLED = { "core", "FastLED/src" };

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

static std::string path_of(const std::string& name) {
    return g_root + "/" + name;
}

static void write_file_at(const std::string& path, const std::string& text, int64_t modified_ns) {
    std::string error;
    FILE* file = make_directories(parent_directory(path), error) ? fopen(path.c_str(), "wb") : nullptr;
    if (!file || fwrite(text.data(), 1, text.size(), file) != text.size()) {
        printf("  cannot write %s\n", path.c_str());
        exit(1);
    }

    fclose(file);
    timespec times[2] = { { modified_ns / NS_PER_SECOND, modified_ns % NS_PER_SECOND },
                          { modified_ns / NS_PER_SECOND, modified_ns % NS_PER_SECOND } };
    utimensat(AT_FDCWD, path.c_str(), times, 0);
}

static void write_text(const std::string& name, const std::string& text, int64_t modified_ns) {
    write_file_at(path_of(name), text, modified_ns);
}

static probe make_probe(const std::string& source, const std::string& depfile,
                        const std::vector<std::string>& include_directories) {
    std::vector<std::string> words = { path_of("bin/g++"), "-w", "-E", "@" + path_of("build/file_opts") };
    probe made = { path_of(source), path_of(depfile), words, "" };
    for (const std::string& directory : include_directories) {
        made.words.push_back("-I" + path_of(directory));
    }

    made.words.insert(made.words.end(), { path_of(source), "-o", "/dev/null", "-MMD", "-MF", path_of(depfile) });
    return made;
}

// FastLED's Blink as discovery reaches it: the sketch misses FastLED.h, then completes, then FastLED's file completes
static std::vector<reached_probe> blink_discovery() {
    return {
        { make_probe("build/sketch/Blink.ino.cpp.merged", "build/sketch/Blink.ino.cpp.merged.libsdetect.d", CORE_ONLY),
          { probe_status::missing, "FastLED.h", "fatal error: FastLED.h: No such file or directory" } },
        { make_probe("build/sketch/Blink.ino.cpp.merged", "build/sketch/Blink.ino.cpp.merged.libsdetect.d",
                     WITH_FASTLED),
          { probe_status::complete, "", "" } },
        { make_probe("FastLED/src/FastLED.cpp", "build/libraries/FastLED/FastLED.cpp.libsdetect.d", WITH_FASTLED),
          { probe_status::complete, "", "" } },
    };
}

static void write_blink_tree() {
    write_text("bin/g++", "compiler", PAST_NS);
    write_text("build/file_opts", "", PAST_NS);
    write_text("core/Arduino.h", "", PAST_NS);
    write_text("FastLED/src/FastLED.h", "", PAST_NS);
    write_text("FastLED/src/FastLED.cpp", "", PAST_NS);
    write_text("build/sketch/Blink.ino.cpp.merged", "", PAST_NS);

    write_text("build/sketch/Blink.ino.cpp.merged.libsdetect.d",
               "/dev/null: " + path_of("build/sketch/Blink.ino.cpp.merged") + " " + path_of("core/Arduino.h") + " " +
                   path_of("FastLED/src/FastLED.h") + "\n",
               PAST_NS + NS_PER_SECOND);
    write_text("build/libraries/FastLED/FastLED.cpp.libsdetect.d",
               "/dev/null: " + path_of("FastLED/src/FastLED.cpp") + " " + path_of("FastLED/src/FastLED.h") + "\n",
               PAST_NS);
}

// How many of the recorded discovery's turns a fresh replay answers, in order, with the results it gave
static size_t answered_turns(std::vector<probe_result>* results = nullptr) {
    discovery_replay replay;
    std::string error;
    if (!replay.load(path_of("build/libraries.record"), error)) {
        return 0;
    }

    size_t answered = 0;
    for (const reached_probe& reached : blink_discovery()) {
        probe_result result;
        if (replay.answer(reached.turn, result)) {
            answered++;
            if (results) {
                results->push_back(result);
            }
        }
    }

    return answered;
}

static void check_replay() {
    std::string error;
    write_blink_tree();
    check(answered_turns() == 0, "answers nothing without a record");
    check(save_discovery_record(path_of("build/libraries.record"), blink_discovery(), error), "saves the record");

    std::vector<probe_result> results;
    check(answered_turns(&results) == 3, "answers every turn of an unchanged discovery");
    check(results.size() == 3 && results[0].status == probe_status::missing && results[0].header == "FastLED.h" &&
              results[1].status == probe_status::complete && results[2].status == probe_status::complete,
          "gives each turn its recorded result");

    discovery_replay replay;
    probe_result result;
    replay.load(path_of("build/libraries.record"), error);
    std::vector<reached_probe> turns = blink_discovery();
    check(!replay.answer(turns[1].turn, result), "answers only the record's next turn");
    check(!replay.answer(turns[0].turn, result), "drops the rest of the record once a turn goes unanswered");
}

static void check_changes() {
    write_blink_tree();
    write_text("build/file_opts", "", PAST_NS + 9 * NS_PER_SECOND);
    check(answered_turns() == 3, "answers when a response file's time moved but not what it holds");

    write_text("build/file_opts", "-DARDUINO_CORE_BUILD", PAST_NS);
    check(answered_turns() == 0, "stops when a response file changed");

    write_blink_tree();
    write_text("FastLED/src/FastLED.cpp", "", PAST_NS + 2 * NS_PER_SECOND);
    check(answered_turns() == 2, "probes again from the turn of a file newer than its .libsdetect.d");

    write_blink_tree();
    write_text("FastLED/src/FastLED.h", "", PAST_NS + 2 * NS_PER_SECOND);
    check(answered_turns() == 0, "stops at a header newer than the .libsdetect.d listing it");

    write_blink_tree();
    write_text("FastLED/src/FastLED.h", "", PAST_NS + NS_PER_SECOND);
    write_text("build/libraries/FastLED/FastLED.cpp.libsdetect.d",
               "/dev/null: " + path_of("FastLED/src/FastLED.cpp") + " " + path_of("FastLED/src/FastLED.h") + "\n",
               PAST_NS + NS_PER_SECOND);
    check(answered_turns() == 3, "answers when a header is exactly as old as the .libsdetect.d files");

    write_blink_tree();
    write_text("build/libraries/FastLED/FastLED.cpp.libsdetect.d",
               "/dev/null: " + path_of("FastLED/src/Other.cpp") + "\n", PAST_NS);
    check(answered_turns() == 2, "stops at a .libsdetect.d naming another source first");

    write_blink_tree();
    write_text("bin/g++", "compiler 2", PAST_NS);
    check(answered_turns() == 0, "stops when the compiler changed");

    write_blink_tree();
    write_text("bin/g++", "compilex", PAST_NS + 3 * NS_PER_SECOND);
    check(answered_turns() == 0, "stops when the compiler's time moved, though not its size");

    write_blink_tree();
    write_text("build/libraries.record", "probe zz\n", PAST_NS);
    check(answered_turns() == 0, "answers nothing from a damaged record");

    std::string error;
    write_blink_tree();
    unlink(path_of("build/file_opts").c_str());
    save_discovery_record(path_of("build/libraries.record"), blink_discovery(), error);
    write_text("build/file_opts", "", PAST_NS);
    check(answered_turns() == 0, "stops at a response file the record kept no stamp for");
}

static library made_library(const std::string& directory, const std::string& name, const std::string& header) {
    library made = {};
    made.name = name;
    made.version = "1.0";
    made.directory = directory;
    made.folder_name = file_name(directory);

    made.location = library_location::installed;
    made.layout = library_layout::recursive;
    made.architectures = { "*" };
    made.headers = { header };
    return made;
}

static property_table probe_properties() {
    property_table properties;
    properties.set("recipe.cpp.o.pattern", "\"g++\" -MMD -c {includes} \"{source_file}\" -o \"{object_file}\"");
    return properties;
}

// What the stand-in preprocessor answers: the header missing until a probe's `-I` words provide it, and once a file
// completes, its .libsdetect.d listing it and that header, as GCC's `-MMD` writes it
static probe_result preprocess(const probe& asked, const std::vector<stand_in>& sources) {
    std::string listed = asked.source;
    for (const stand_in& known : sources) {
        if (known.source != asked.source || known.header.empty()) {
            continue;
        }

        bool provided = std::find(asked.words.begin(), asked.words.end(), "-I" + known.providing) != asked.words.end();
        if (!provided) {
            return { probe_status::missing, known.header, "fatal error: " + known.header + ": No such file" };
        }

        listed += " " + known.providing + "/" + known.header;
    }

    write_file_at(asked.depfile, "/dev/null: " + listed + "\n", PROBED_NS);
    return { probe_status::complete, "", "" };
}

// Discovery as the build runs it: the record answers turns until its first miss, then a file that completes alone
// lets later files probe ahead. Counts the probes the stand-in ran and names the source of the first.
static bool run_discovery(library_discovery& discovery, discovery_replay& replay, const std::vector<stand_in>& sources,
                          size_t& probes_run, std::string& first_probed) {
    std::map<std::vector<std::string>, probe_result> ahead;
    bool ahead_started = false;
    probes_run = 0;
    first_probed.clear();
    while (const probe* turn = discovery.turn_probe()) {
        probe_result result;
        bool replayed = replay.answer(*turn, result);
        auto found = ahead.find(turn->words);
        bool ran_alone = !replayed && found == ahead.end();
        if (!replayed && first_probed.empty()) {
            first_probed = turn->source;
        }

        if (ran_alone) {
            ahead.clear();
            result = preprocess(*turn, sources);
            probes_run++;
        } else if (!replayed) {
            result = found->second;
        }

        discovery_failure failure;
        if (!discovery.take_turn_result(result, failure)) {
            return false;
        }

        bool completed = result.status == probe_status::complete;
        ahead_started = ahead_started || (ran_alone && completed);
        if (!ahead_started || !completed) {
            continue;
        }

        for (const probe& later : discovery.probes_ahead(AHEAD_SLOTS)) {
            ahead[later.words] = preprocess(later, sources);
            probes_run++;
        }
    }

    return true;
}

static void check_rebuilds() {
    std::string build = path_of("rebuild/build");
    std::string display = path_of("rebuild/libs/Display");
    std::string bus = path_of("rebuild/libs/Bus");
    std::string merged = build + "/sketch/Demo.ino.cpp.merged";
    std::string record = build + "/" + DISCOVERY_RECORD_NAME;
    for (const std::string& file : { merged, build + "/sketch/extra.cpp", display + "/src/Display.h",
                                     display + "/src/Display.cpp", bus + "/src/Bus.h", bus + "/src/Bus.cpp" }) {
        write_file_at(file, "", PAST_NS);
    }

    std::vector<library> libraries = { made_library(display, "Display", "Display.h"),
                                       made_library(bus, "Bus", "Bus.h") };
    std::vector<stand_in> sources = { { merged, "Display.h", display + "/src" },
                                      { display + "/src/Display.cpp", "Bus.h", bus + "/src" } };
    std::vector<std::string> discovered = { merged, build + "/sketch/extra.cpp" };

    std::string error;
    std::string texts[3];
    std::string first_probed;
    size_t probes_run[3] = {};
    for (size_t run = 0; run < 3; run++) {
        if (run == 2) {
            write_file_at(bus + "/src/Bus.cpp", "// edited", EDITED_NS);
        }

        discovery_replay replay;
        library_discovery discovery(probe_properties(), libraries, "esp32", build, discovered, { "/core" });
        bool finished = replay.load(record, error) &&
                        run_discovery(discovery, replay, sources, probes_run[run], first_probed) &&
                        save_discovery_record(record, discovery.reached(), error) &&
                        read_file(record, texts[run], error);
        check(finished && discovery.libraries_added().size() == 2, "each build finds both libraries");
    }

    check(probes_run[0] > 0 && probes_run[1] == 0, "an untouched rebuild runs no probe");
    check(texts[1] == texts[0], "and writes the same record");
    check(probes_run[2] > 0 && first_probed == bus + "/src/Bus.cpp", "an edited library file is probed from its turn");
    check(texts[2] == texts[0], "and the record it writes holds the same turns");
}

int main() {
    char root[] = "/tmp/mcufw-replay-XXXXXX";
    if (!mkdtemp(root)) {
        printf("  cannot make a temporary folder\n");
        return 1;
    }

    g_root = root;
    check_replay();
    check_changes();
    check_rebuilds();
    std::string error;
    if (!remove_tree(g_root, error)) {
        printf("  %s\n", error.c_str());
    }

    if (g_failures != 0) {
        printf("discovery_replay: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("discovery_replay: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
