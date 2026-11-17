/* core_cache_key, the sketch guards and publish_core, run on the build machine in temporary build trees: two sketches
 * sharing a core, what gives a core a key of its own or keeps it unshared, and an entry placed whole */

#include "core_cache.hpp"

#include "files.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

static const char* const COMPILER_VERSION = "xtensa-esp32-elf-g++ (crosstool-NG esp-14.2.0_20251107) 14.2.0";

static uint64_t    g_checks;
static uint64_t    g_failures;
static std::string g_root;

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

static void write_text(const std::string& name, const std::string& text) {
    std::string path = path_of(name);
    std::string error;
    FILE* file = make_directories(parent_directory(path), error) ? fopen(path.c_str(), "wb") : nullptr;
    if (!file || fwrite(text.data(), 1, text.size(), file) != text.size()) {
        printf("  cannot write %s\n", path.c_str());
        exit(1);
    }

    fclose(file);
}

// The core steps of a sketch built in `build`, as ESP32's recipes write them, with the sketch folder on the include
// path and the core's hooks writing file_opts
static std::vector<build_step> core_steps(const std::string& build, const std::string& sketch,
                                          const std::string& archiver, const std::string& core_define) {
    std::string compiler = "/var/lib/mcufw/tools/esp32/esp-x32/2601/bin/xtensa-esp32-elf-g++";
    std::vector<build_step> steps;
    steps.push_back({ step_kind::hook, "recipe.hooks.core.prebuild.1.pattern",
                      { { "mcufw", "hook", "write", build + "/file_opts", core_define } }, "", "", {} });
    for (const char* source : { "main.cpp", "Print.cpp" }) {
        std::string object = build + "/core/" + source + ".o";
        steps.push_back({ step_kind::compile, "recipe.cpp.o.pattern",
                          { { compiler, "-MMD", "-c", "@/var/lib/mcufw/tools/esp32/esp32-libs/3.3.12/flags/cpp_flags",
                              "-I" + sketch, "@" + build + "/build_opt.h", "@" + build + "/file_opts",
                              std::string("/var/lib/mcufw/hardware/esp32/esp32/3.3.12/cores/esp32/") + source, "-o",
                              object } },
                          std::string("/var/lib/mcufw/hardware/esp32/esp32/3.3.12/cores/esp32/") + source, object,
                          {} });
    }

    steps.push_back({ step_kind::archive, "recipe.ar.pattern",
                      { { archiver, "cr", build + "/core/core.a", build + "/core/main.cpp.o" },
                        { archiver, "cr", build + "/core/core.a", build + "/core/Print.cpp.o" } },
                      "", build + "/core/core.a", {} });
    steps.push_back({ step_kind::hook, "recipe.hooks.core.postbuild.1.pattern",
                      { { "mcufw", "hook", "write", build + "/file_opts" } }, "", "", {} });
    return steps;
}

static std::string key_of(const std::string& build_name, const std::string& sketch_name, const std::string& build_opt,
                          const std::string& archiver = "xtensa-esp32-elf-ar",
                          const std::string& core_define = "-DARDUINO_CORE_BUILD",
                          const std::string& compiler_version = COMPILER_VERSION) {
    std::string build = path_of(build_name);
    write_text(build_name + "/build_opt.h", build_opt);
    write_text(build_name + "/file_opts", "");

    std::string key;
    std::string error;
    if (!core_cache_key(core_steps(build, path_of(sketch_name), archiver, core_define), build, path_of(sketch_name),
                        compiler_version, key, error)) {
        printf("  %s\n", error.c_str());
    }

    return key;
}

static void check_keys() {
    std::string wifi_scan = key_of("builds/WiFiScan-3f9c2a17", "sketches/WiFiScan", "");
    check(wifi_scan.size() == 16, "keys a core by 16 hex digits");
    check(key_of("builds/Lamp-0a1b2c3d", "sketches/Lamp", "") == wifi_scan,
          "gives two sketches built for the same board one core");
    check(key_of("builds/Blink-4f3247ad", "sketches/Blink", "-DPIN_DATA=25") != wifi_scan,
          "gives a sketch whose build_opt.h differs a core of its own");

    check(key_of("builds/Lamp-0a1b2c3d", "sketches/Lamp", "", "xtensa-esp32-elf-gcc-ar") != wifi_scan,
          "keys by the archive command");
    check(key_of("builds/Lamp-0a1b2c3d", "sketches/Lamp", "", "xtensa-esp32-elf-ar", "-DOTHER") != wifi_scan,
          "keys by what the core's hooks write");
    check(key_of("builds/Lamp-0a1b2c3d", "sketches/Lamp", "", "xtensa-esp32-elf-ar", "-DARDUINO_CORE_BUILD",
                 "xtensa-esp32-elf-g++ (crosstool-NG esp-15.1.0) 15.1.0") != wifi_scan,
          "keys by the compiler's version line");

    std::string build = path_of("builds/Lamp-0a1b2c3d");
    std::string key;
    std::string other_key;
    std::string error;

    std::vector<build_step> lamp_neighbor = core_steps(build, path_of("sketches/Lamp"), "ar", "-D");
    lamp_neighbor[1].commands[0][4] = "-I" + path_of("sketches/Lamp2");
    std::vector<build_step> blink_neighbor = core_steps(build, path_of("sketches/Blink"), "ar", "-D");
    blink_neighbor[1].commands[0][4] = "-I" + path_of("sketches/Blink2");
    check(core_cache_key(lamp_neighbor, build, path_of("sketches/Lamp"), COMPILER_VERSION, key, error) &&
              core_cache_key(blink_neighbor, build, path_of("sketches/Blink"), COMPILER_VERSION, other_key, error) &&
              key != other_key,
          "writes only whole path components as the sketch folder");

    std::vector<build_step> quoted_empty = core_steps(build, path_of("sketches/Lamp"), "ar", "-D");
    quoted_empty[1].commands[0].push_back("");
    check(core_cache_key(quoted_empty, build, path_of("sketches/Lamp"), COMPILER_VERSION, key, error),
          "keys a command holding an empty word");

    unlink(path_of("builds/Lamp-0a1b2c3d/build_opt.h").c_str());
    check(!core_cache_key(core_steps(build, path_of("sketches/Lamp"), "ar", "-D"), build, path_of("sketches/Lamp"),
                          COMPILER_VERSION, key, error),
          "fails when a response file in the build tree is missing");

    directory_layout layout;
    layout.core_cache = "/var/cache/mcufw/cores";
    check(core_cache_entry(layout, "esp32da", "6c1f0e2a9b4d7735") == "/var/cache/mcufw/cores/esp32da-6c1f0e2a9b4d7735",
          "names an entry by board and key");
}

static void check_sketch_guard() {
    std::string build = path_of("builds/Lamp-0a1b2c3d");
    std::vector<build_step> steps = core_steps(build, path_of("sketches/Lamp"), "ar", "-D");
    std::vector<build_step> compiles = { steps[1], steps[2] };
    write_text("builds/Lamp-0a1b2c3d/core/main.cpp.d",
               build + "/core/main.cpp.o: /var/lib/mcufw/hardware/esp32/esp32/3.3.12/cores/esp32/main.cpp\n");
    write_text("builds/Lamp-0a1b2c3d/core/Print.cpp.d",
               build + "/core/Print.cpp.o: /var/lib/mcufw/hardware/esp32/esp32/3.3.12/cores/esp32/Print.cpp " +
                   path_of("sketches/Lamp2/Print.h") + "\n");

    bool reads = true;
    std::string error;
    check(core_reads_sketch(compiles, path_of("sketches/Lamp"), true, reads, error) && !reads,
          "shares a core that reads nothing from the sketch folder");

    write_text("builds/Lamp-0a1b2c3d/core/Print.cpp.d",
               build + "/core/Print.cpp.o: /var/lib/mcufw/hardware/esp32/esp32/3.3.12/cores/esp32/Print.cpp " +
                   path_of("sketches/Lamp/Print.h") + "\n");
    check(core_reads_sketch(compiles, path_of("sketches/Lamp"), true, reads, error) && reads,
          "never shares a core that read a header from the sketch folder");
    check(core_reads_sketch(compiles, path_of("sketches/Lamp"), false, reads, error) && reads,
          "never links a cached core into a tree whose core read the sketch folder");

    unlink(path_of("builds/Lamp-0a1b2c3d/core/Print.cpp.d").c_str());
    check(core_reads_sketch(compiles, path_of("sketches/Lamp"), true, reads, error) && reads,
          "never shares a core with a compile that wrote no .d file");
    check(core_reads_sketch(compiles, path_of("sketches/Lamp"), false, reads, error) && !reads,
          "links a cached core into a tree whose compiles wrote no .d file yet");
}

static void check_shadowing() {
    std::string build = path_of("builds/Lamp-0a1b2c3d");
    std::vector<build_step> steps = core_steps(build, path_of("sketches/Lamp"), "ar", "-D");
    std::vector<build_step> compiles = { steps[1], steps[2] };
    std::string main_depends = "/cores/esp32/main.cpp /cores/esp32/Arduino.h /variants/esp32da/pins_arduino.h";
    write_text("builds/Lamp-0a1b2c3d/core/main.cpp.d", build + "/core/main.cpp.o: " + main_depends + "\n");
    write_text("builds/Lamp-0a1b2c3d/core/Print.cpp.d",
               build + "/core/Print.cpp.o: /cores/esp32/Print.cpp /cores/esp32/Print.h /cores/esp32/Arduino.h\n");

    std::vector<std::string> names;
    std::string error;
    check(core_header_names(compiles, names, error) &&
              names == std::vector<std::string>({ "Arduino.h", "Print.h", "pins_arduino.h" }),
          "names each header the core read once, its sources left out");

    bool shadows = true;
    write_text("sketches/Lamp/helpers.h", "");
    check(sketch_shadows(path_of("sketches/Lamp"), names, shadows, error) && !shadows,
          "a sketch whose files share no name with those headers shadows none");

    write_text("sketches/Lamp/pins_arduino.h", "");
    check(sketch_shadows(path_of("sketches/Lamp"), names, shadows, error) && shadows,
          "a sketch holding a file named like one shadows it");
}

static void check_publish() {
    std::string entry = path_of("cores/esp32da-6c1f0e2a9b4d7735");
    std::string error;
    std::vector<std::string> headers = { "Arduino.h", "pins_arduino.h" };
    std::vector<std::string> kept;
    check(!published_header_names(entry, kept), "an entry not placed keeps no header names");

    write_text("builds/WiFiScan-3f9c2a17/core/core.a", "first");
    check(publish_core(path_of("builds/WiFiScan-3f9c2a17/core/core.a"), headers, entry, error), "publishes a core");
    check(published_header_names(entry, kept) && kept == headers, "keeps the names of its headers beside it");

    std::string text;
    check(read_file(entry + "/core.a", text, error) && text == "first", "moves the archive into its entry");
    check(!is_file(path_of("builds/WiFiScan-3f9c2a17/core/core.a")), "leaves no archive in the build tree");

    write_text("builds/Lamp-0a1b2c3d/core/core.a", "second");
    check(publish_core(path_of("builds/Lamp-0a1b2c3d/core/core.a"), headers, entry, error),
          "publishes a core placed before");
    check(read_file(entry + "/core.a", text, error) && text == "first", "keeps the entry placed first");
    check(!is_file(path_of("builds/Lamp-0a1b2c3d/core/core.a")), "deletes the archive it did not need");

    std::vector<std::string> names;
    bool listed = list_directory(path_of("cores"), names, error);
    check(listed && names == std::vector<std::string>{ "esp32da-6c1f0e2a9b4d7735" }, "leaves no staging folder behind");

    std::string missing_entry = path_of("cores/esp32c3-0f1e2d3c4b5a6978");
    check(!publish_core(path_of("builds/Gone-5e6f7a8b/core/core.a"), headers, missing_entry, error) &&
              !is_directory(missing_entry),
          "fails to publish an archive that is missing");
}

int main() {
    char root[] = "/tmp/mcufw-cores-XXXXXX";
    if (!mkdtemp(root)) {
        printf("  cannot make a temporary folder\n");
        return 1;
    }

    g_root = root;
    check_keys();
    check_sketch_guard();
    check_shadowing();
    check_publish();

    std::string error;
    if (!remove_tree(g_root, error)) {
        printf("  %s\n", error.c_str());
    }

    if (g_failures != 0) {
        printf("core_cache: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("core_cache: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
