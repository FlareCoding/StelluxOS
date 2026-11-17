/* Host unit tests for Sheen's pure C++20 embedded protocol parsers and workspace models. */

#include "extensions/embedded/embedded_protocol.h"

#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

static constexpr uint32_t MAX_FAILURE_REPORTS = 12;

static uint64_t g_checks = 0;
static uint64_t g_failures = 0;

static void check(bool condition, const std::string& description) {
    g_checks++;
    if (condition) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_FAILURE_REPORTS) {
        std::printf("  FAIL: %s\n", description.c_str());
    }
}

static std::string make_temp_dir() {
    const char* base_env = std::getenv("TMPDIR");
    std::string prefix = base_env != nullptr && base_env[0] != '\0' ? std::string(base_env) : std::string("/tmp");
    if (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }

    std::string pattern = prefix + "/sheen-embedded-test-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = mkdtemp(writable.data());
    check(created != nullptr, "temporary test directory is created");
    if (created == nullptr) {
        return {};
    }

    char resolved[PATH_MAX];
    return realpath(created, resolved) != nullptr ? std::string(resolved) : std::string(created);
}

static void write_file(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

static void check_dependency_probes_and_fqbn() {
    check(std::string_view(sheen::EMBEDDED_PYTHON_PROBE_PATH) == "/bin/python3",
          "python dependency probes /bin/python3 installed by packages/python");
    check(std::string_view(sheen::EMBEDDED_ESPTOOL_PROBE_PATH) ==
              "/usr/lib/python3.12/site-packages/esptool/__init__.py",
          "esptool dependency probes Python 3.12 site-packages");
    check(std::string_view(sheen::EMBEDDED_XTENSA_GPP_PROBE_PATH) ==
              "/usr/xtensa-esp32-elf/bin/xtensa-esp32-elf-g++",
          "xtensa toolchain probes xtensa-esp32-elf-g++");

    std::string_view core_target(sheen::EMBEDDED_DEFAULT_CORE_TARGET);
    size_t colons = 0;
    for (char ch : core_target) {
        if (ch == ':') {
            colons++;
        }
    }

    check(core_target == "esp32:esp32:esp32" && colons == 2,
          "default ESP32 core install target is 3-part board FQBN so mcufw installs esp32-libs");
    check(std::string_view(sheen::EMBEDDED_ESP32_LIBS_PROBE_PATH) == "/var/lib/mcufw/tools/esp32/esp32-libs",
          "ESP32 core post-install probe checks /var/lib/mcufw/tools/esp32/esp32-libs");

    check(sheen::base_fqbn("esp32:esp32:esp32da") == "esp32:esp32:esp32da",
          "3-part FQBN stays unchanged");
    check(sheen::base_fqbn("  esp32:esp32:esp32da:UploadSpeed=921600,FlashSize=8M ") == "esp32:esp32:esp32da",
          "4-part FQBN strips menu option suffix and whitespace");
    check(sheen::base_fqbn("esp32:esp32") == "esp32:esp32",
          "2-part core identifier stays intact");
}

static void check_board_catalog_preservation() {
    sheen::board_catalog catalog;
    check(catalog.boards().empty(), "board catalog starts empty");

    catalog.begin_refresh();
    catalog.feed_stdout("board\tesp32:esp32:esp32da\t-\tESP32-WROOM-DA Module\n");
    catalog.feed_stdout("board\tesp32:esp32:esp32s3\txtensa-esp32s3-elf-g++\tESP32S3 Dev Module");
    check(catalog.finish_refresh(0, false) && catalog.boards().size() == 2,
          "clean exit code 0 commits streamed and flushed boards");

    const sheen::board_entry* found = catalog.find_by_fqbn("esp32:esp32:esp32da:FlashSize=8M");
    check(found != nullptr && found->name == "ESP32-WROOM-DA Module" && found->needs.empty(),
          "find_by_fqbn matches 4-part FQBN against 3-part catalog entry");

    catalog.begin_refresh();
    catalog.feed_stdout("board\tpartial:only:board\t-\tPartial\n");
    check(!catalog.finish_refresh(1, false), "non-zero exit code rejects partial board refresh");
    check(catalog.boards().size() == 2 && catalog.boards()[0].fqbn == "esp32:esp32:esp32da",
          "board list is preserved intact when mcufw boards exits non-zero");

    catalog.begin_refresh();
    catalog.feed_stdout("board\tstopped:only:board\t-\tStopped\n");
    check(!catalog.finish_refresh(0, true) && catalog.boards().size() == 2,
          "stopped board refresh preserves previous board list");

    catalog.begin_refresh();
    catalog.feed_stdout("failed\tlock\tstate lock busy\n");
    check(!catalog.finish_refresh(0, false) && catalog.boards().size() == 2,
          "failed event during board refresh preserves previous board list");
}

static void check_mcufw_events() {
    sheen::mcufw_event listed_ok =
        sheen::parse_mcufw_event_line("board\tesp32:esp32:esp32da\t-\tESP32-WROOM-DA Module\n");
    check(listed_ok.kind == sheen::mcufw_event_kind::BOARD
              && listed_ok.fqbn == "esp32:esp32:esp32da"
              && listed_ok.needs.empty()
              && listed_ok.name == "ESP32-WROOM-DA Module",
          "4-field board event with '-' needs parses with empty needs");

    sheen::mcufw_event listed_needs = sheen::parse_mcufw_event_line(
        "board\tesp32:esp32:esp32s3-octal\txtensa-esp32s3-elf-g++, esp32:esp32s3-libs 3.3.12\tESP32S3 Octal\n");
    check(listed_needs.kind == sheen::mcufw_event_kind::BOARD
              && listed_needs.needs == "xtensa-esp32s3-elf-g++, esp32:esp32s3-libs 3.3.12"
              && listed_needs.name == "ESP32S3 Octal",
          "4-field board event preserves missing tool list");

    sheen::mcufw_event selected_board = sheen::parse_mcufw_event_line(
        "board\tesp32:esp32:esp32da:UploadSpeed=921600\tESP32-WROOM-DA Module\n");
    check(selected_board.kind == sheen::mcufw_event_kind::BOARD
              && selected_board.fqbn == "esp32:esp32:esp32da:UploadSpeed=921600"
              && selected_board.needs.empty()
              && selected_board.name == "ESP32-WROOM-DA Module",
          "3-field build board_selected event parses cleanly");

    check(sheen::parse_mcufw_event_line("board\t\t-\tName").kind == sheen::mcufw_event_kind::UNKNOWN,
          "board event with empty FQBN is rejected");
    check(sheen::parse_mcufw_event_line("board\tonly_fqbn").kind == sheen::mcufw_event_kind::UNKNOWN,
          "2-field board event is rejected");

    sheen::mcufw_event lib_ev = sheen::parse_mcufw_event_line("library\tWiFi\t3.3.12\t/cores/esp32/libraries/WiFi\n");
    check(lib_ev.kind == sheen::mcufw_event_kind::LIBRARY
              && lib_ev.name == "WiFi"
              && lib_ev.version == "3.3.12"
              && lib_ev.folder == "/cores/esp32/libraries/WiFi",
          "library event parses name, version, and directory");

    sheen::mcufw_event err_prob = sheen::parse_mcufw_event_line(
        "problem\terror\t/sketches/WiFiScan/WiFiScan.ino\t10\t5\texpected ';' before 'Serial'\n");
    check(err_prob.kind == sheen::mcufw_event_kind::PROBLEM
              && err_prob.severity == sheen::problem_level::ERROR
              && err_prob.file_path == "/sketches/WiFiScan/WiFiScan.ino"
              && err_prob.line == 10
              && err_prob.column == 5
              && err_prob.message == "expected ';' before 'Serial'",
          "error problem event parses severity, file, line, column, and message");

    sheen::mcufw_event warn_prob = sheen::parse_mcufw_event_line(
        "problem\twarning\t/sketches/Blink/Blink.ino\t4\t0\tunused variable 'x'\n");
    check(warn_prob.kind == sheen::mcufw_event_kind::PROBLEM
              && warn_prob.severity == sheen::problem_level::WARNING
              && warn_prob.line == 4
              && warn_prob.column == 0,
          "warning problem event with column 0 parses");

    sheen::mcufw_event note_prob = sheen::parse_mcufw_event_line(
        "problem\tnote\t/cores/Arduino.h\t42\t1\tdeclared here\n");
    check(note_prob.kind == sheen::mcufw_event_kind::PROBLEM && note_prob.severity == sheen::problem_level::NOTE,
          "note problem event parses as NOTE");

    check(sheen::parse_mcufw_event_line("problem\tfatal\t/a.ino\t1\t1\tmsg").kind == sheen::mcufw_event_kind::UNKNOWN,
          "unknown problem severity is rejected");
    check(sheen::parse_mcufw_event_line("problem\terror\t/a.ino\t0\t1\tmsg").kind == sheen::mcufw_event_kind::UNKNOWN,
          "problem event with line 0 is rejected");
    check(sheen::parse_mcufw_event_line("problem\terror\t/a.ino\tabc\t1\tmsg").kind == sheen::mcufw_event_kind::UNKNOWN,
          "problem event with non-numeric line is rejected");

    sheen::mcufw_event hint_ev = sheen::parse_mcufw_event_line("hint\tFastLED.h\tFastLED\t3.10.9\n");
    check(hint_ev.kind == sheen::mcufw_event_kind::HINT
              && hint_ev.header == "FastLED.h"
              && hint_ev.name == "FastLED"
              && hint_ev.version == "3.10.9",
          "hint event parses header, library name, and version");

    sheen::mcufw_event flash_size = sheen::parse_mcufw_event_line("size\tflash\t314572\t1310720\n");
    check(flash_size.kind == sheen::mcufw_event_kind::SIZE
              && flash_size.bank == sheen::memory_bank::FLASH
              && flash_size.used_bytes == 314572
              && flash_size.max_bytes == 1310720
              && flash_size.has_max_bytes,
          "size flash event parses used and maximum bytes");

    sheen::mcufw_event unbounded_size = sheen::parse_mcufw_event_line("size\tram\t45680\t-\n");
    check(unbounded_size.kind == sheen::mcufw_event_kind::SIZE
              && unbounded_size.bank == sheen::memory_bank::RAM
              && unbounded_size.used_bytes == 45680
              && !unbounded_size.has_max_bytes,
          "size event with '-' maximum marks has_max_bytes false");

    sheen::mcufw_event zero_max_size = sheen::parse_mcufw_event_line("size\tflash\t1024\t0\n");
    check(zero_max_size.kind == sheen::mcufw_event_kind::SIZE && !zero_max_size.has_max_bytes,
          "size event with 0 maximum marks has_max_bytes false");

    check(sheen::parse_mcufw_event_line("size\teeprom\t10\t100").kind == sheen::mcufw_event_kind::UNKNOWN,
          "size event with unknown bank is rejected");

    sheen::mcufw_event prog = sheen::parse_mcufw_event_line("progress\tcompile\t31\t75\n");
    check(prog.kind == sheen::mcufw_event_kind::PROGRESS
              && prog.stage == "compile"
              && prog.done == 31
              && prog.total == 75,
          "progress event parses stage, done, and total");

    sheen::mcufw_event log_ev = sheen::parse_mcufw_event_line("log\tcompile\tCompiling Blink.ino\n");
    check(log_ev.kind == sheen::mcufw_event_kind::LOG
              && log_ev.stage == "compile"
              && log_ev.message == "Compiling Blink.ino",
          "log event parses stage and text");

    sheen::mcufw_event start_ev = sheen::parse_mcufw_event_line("start\tesp32:esp32\t3.3.12\t51102895\n");
    check(start_ev.kind == sheen::mcufw_event_kind::START
              && start_ev.name == "esp32:esp32"
              && start_ev.version == "3.3.12"
              && start_ev.total == 51102895,
          "start event parses item, version, and total bytes");

    sheen::mcufw_event inst_ev = sheen::parse_mcufw_event_line("installed\tFastLED\t3.10.9\n");
    check(inst_ev.kind == sheen::mcufw_event_kind::INSTALLED
              && inst_ev.name == "FastLED"
              && inst_ev.version == "3.10.9",
          "installed event parses item and version");

    sheen::mcufw_event needs_ev = sheen::parse_mcufw_event_line("needs\txtensa-esp32s3-elf-g++\t-\n");
    check(needs_ev.kind == sheen::mcufw_event_kind::NEEDS
              && needs_ev.name == "xtensa-esp32s3-elf-g++"
              && needs_ev.version.empty(),
          "needs event clears '-' version");

    sheen::mcufw_event warn_ev = sheen::parse_mcufw_event_line("warning\tcannot download library index\n");
    check(warn_ev.kind == sheen::mcufw_event_kind::WARNING && warn_ev.reason == "cannot download library index",
          "warning event parses reason");

    sheen::mcufw_event built_ev = sheen::parse_mcufw_event_line("built\t/var/cache/mcufw/builds/k/Blink.ino.bin\n");
    check(built_ev.kind == sheen::mcufw_event_kind::BUILT
              && built_ev.file_path == "/var/cache/mcufw/builds/k/Blink.ino.bin",
          "built event parses output binary path");

    sheen::mcufw_event failed_ev =
        sheen::parse_mcufw_event_line("failed\tlibrary\tno installed library provides FastLED.h\n");
    check(failed_ev.kind == sheen::mcufw_event_kind::FAILED
              && failed_ev.failure_kind == "library"
              && failed_ev.reason == "no installed library provides FastLED.h",
          "failed event parses failure kind and reason");

    check(sheen::parse_mcufw_event_line("future_fact\t1\t2").kind == sheen::mcufw_event_kind::UNKNOWN,
          "unknown future mcufw event kind is skipped cleanly");
}

static void check_memory_and_progress_formatting() {
    sheen::memory_usage usage;
    check(sheen::format_memory_summary(usage).empty() && !sheen::is_memory_usage_danger(usage),
          "empty memory usage formats empty string");

    sheen::record_memory_size(usage, sheen::parse_mcufw_event_line("size\tflash\t314573\t1310720\n"));
    check(sheen::format_memory_summary(usage) == "Flash 24%", "flash-only memory readout");

    sheen::record_memory_size(usage, sheen::parse_mcufw_event_line("size\tram\t29492\t327680\n"));
    check(sheen::format_memory_summary(usage) == "Flash 24% \xC2\xB7 RAM 9%",
          "combined Flash and RAM percentage readout");
    check(!sheen::is_memory_usage_danger(usage), "24% Flash and 9% RAM is not danger");

    sheen::memory_usage boundary_89;
    sheen::record_memory_size(boundary_89, sheen::parse_mcufw_event_line("size\tflash\t89\t100\n"));
    check(!sheen::is_memory_usage_danger(boundary_89), "89% usage stays below danger threshold");

    sheen::memory_usage boundary_90;
    sheen::record_memory_size(boundary_90, sheen::parse_mcufw_event_line("size\tflash\t90\t100\n"));
    check(sheen::is_memory_usage_danger(boundary_90), "90% Flash usage triggers danger highlight");

    sheen::memory_usage ram_only;
    sheen::record_memory_size(ram_only, sheen::parse_mcufw_event_line("size\tram\t92\t100\n"));
    check(sheen::format_memory_summary(ram_only) == "RAM 92%" && sheen::is_memory_usage_danger(ram_only),
          "RAM-only usage formats cleanly and triggers danger at >=90%");

    sheen::memory_usage no_max;
    sheen::record_memory_size(no_max, sheen::parse_mcufw_event_line("size\tflash\t1023\t0\n"));
    sheen::record_memory_size(no_max, sheen::parse_mcufw_event_line("size\tram\t1024\t-\n"));
    check(sheen::format_memory_summary(no_max) == "Flash 1023 B \xC2\xB7 RAM 1.0 KB"
              && !sheen::is_memory_usage_danger(no_max),
          "exact 1023 B vs 1024 B boundary and zero max format without danger");

    sheen::memory_usage mb_usage;
    sheen::record_memory_size(mb_usage, sheen::parse_mcufw_event_line("size\tflash\t1572864\t-\n"));
    check(sheen::format_memory_summary(mb_usage) == "Flash 1.5 MB", "megabyte quantity formatting");

    check(sheen::compute_progress_percent(0, 0) == -1, "zero total gives indeterminate -1 progress");
    check(sheen::compute_progress_percent(30, 75) == 40, "compile progress percentage");
    check(sheen::compute_progress_percent(99, 100) == 99, "99 of 100 stays 99%");
    check(sheen::compute_progress_percent(100, 100) == 100, "100 of 100 gives 100%");
    check(sheen::compute_progress_percent(999, 100) == 100, "progress clamps at 100%");

    check(sheen::format_progress_label("compile", 12, 75, false) == "Building 12/75", "compile progress label");
    check(sheen::format_progress_label("compile", 0, 0, false) == "Building", "compile zero-total label");
    check(sheen::format_progress_label("libraries", 1, 3, false) == "Resolving libraries", "libraries stage label");
    check(sheen::format_progress_label("link", 1, 2, false) == "link 1/2", "generic stage progress label");

    check(sheen::format_progress_label("FastLED", 20, 40, true) == "Installing FastLED 50%",
          "install progress label");
    check(sheen::format_progress_label("FastLED", 0, 0, true) == "Installing FastLED",
          "install zero-total label");
}

static void check_diagnostics_and_library_hints(const std::string& temp_root) {
    std::vector<sheen::embedded_diagnostic> diagnostics;
    check(!sheen::append_problem_diagnostic(
              diagnostics, sheen::parse_mcufw_event_line("problem\tnote\t/a.h\t2\t1\tincluded from here\n")),
          "compiler note does not create inline squiggle diagnostic");

    check(sheen::append_problem_diagnostic(
              diagnostics, sheen::parse_mcufw_event_line(
                               "problem\terror\t/sketches/Blink/Blink.ino\t1\t10\tFastLED.h: No such file or directory\n")),
          "compiler error creates diagnostic");
    check(sheen::append_problem_diagnostic(
              diagnostics, sheen::parse_mcufw_event_line(
                               "problem\twarning\t/sketches/Blink/Blink.ino\t8\t5\tunused variable 'led'\n")),
          "compiler warning creates diagnostic");

    sheen::mcufw_event hint = sheen::parse_mcufw_event_line("hint\tFastLED.h\tFastLED\t3.10.9\n");
    check(sheen::attach_library_hint(diagnostics, hint) && diagnostics.size() == 2,
          "hint attaches to matching header error rather than trailing warning");
    check(diagnostics[0].fix_library == "FastLED"
              && diagnostics[0].fix_version == "3.10.9"
              && diagnostics[0].fix_header == "FastLED.h"
              && diagnostics[0].message == "FastLED.h: No such file or directory. Provided by FastLED 3.10.9",
          "hint enriches diagnostic message and sets one-click library fix");
    check(diagnostics[1].fix_library.empty(), "warning diagnostic stays untouched by hint");

    check(sheen::attach_library_hint(diagnostics, hint)
              && diagnostics[0].message == "FastLED.h: No such file or directory. Provided by FastLED 3.10.9",
          "re-attaching identical hint does not duplicate Provided by suffix");

    std::filesystem::path fallback_sketch = std::filesystem::path(temp_root) / "HintOnly";
    write_file(fallback_sketch / "HintOnly.ino", "// comment\n  #  include \"Adafruit_SSD1306.h\"\nvoid setup() {}\n");
    write_file(fallback_sketch / "extra.h", "#include \"Adafruit_SSD1306.h\"\n");

    std::vector<sheen::embedded_diagnostic> synthesized;
    sheen::mcufw_event ssd_hint =
        sheen::parse_mcufw_event_line("hint\tAdafruit_SSD1306.h\tAdafruit SSD1306\t2.5.17\n");
    check(sheen::attach_library_hint(synthesized, ssd_hint, fallback_sketch.string()) && synthesized.size() == 1,
          "hint without prior problem locates quoted #include line in sketch, preferring .ino over .h");
    check(synthesized[0].file_path == (fallback_sketch / "HintOnly.ino").string()
              && synthesized[0].line == 2
              && synthesized[0].fix_library == "Adafruit SSD1306",
          "synthesized diagnostic points to exact 1-based #include line");
}

static void check_sketch_yaml_and_workspace_paths(const std::string& temp_root) {
    sheen::sketch_defaults parsed = sheen::parse_sketch_yaml(
        "\xEF\xBB\xBF"
        "default_fqbn: \"esp32:esp32:esp32da:FlashSize=8M\" # desk esp32\r\n"
        "profiles:\r\n"
        "  esp32s3:\r\n"
        "    default_fqbn: wrong:indented:board\r\n");
    check(parsed.fqbn == "esp32:esp32:esp32da:FlashSize=8M",
          "parse_sketch_yaml handles BOM, CRLF, quotes, comments, and ignores indented keys");

    sheen::sketch_defaults single_quoted = sheen::parse_sketch_yaml("default_fqbn: 'esp32:esp32:esp32s3' # note\n");
    check(single_quoted.fqbn == "esp32:esp32:esp32s3",
          "parse_sketch_yaml reads single-quoted FQBN");

    check(sheen::is_path_within_workspace("/sketches/led", "/sketches/led"),
          "workspace root itself is within workspace");
    check(sheen::is_path_within_workspace("/sketches/led/sub/Blink", "/sketches/led/"),
          "nested subdirectory is within workspace with trailing slash");
    check(!sheen::is_path_within_workspace("/sketches/led-v2", "/sketches/led"),
          "sibling directory sharing string prefix without '/' boundary is rejected");
    check(sheen::is_path_within_workspace("/sketches/led", "/") && !sheen::is_path_within_workspace("", "/sketches"),
          "root '/' workspace accepts absolute paths and empty path is rejected");

    std::filesystem::path root_sketch = std::filesystem::path(temp_root) / "Blink";
    write_file(root_sketch / "Blink.ino", "void setup() {}\n");
    write_file(root_sketch / "sketch.yml", "default_fqbn: esp32:esp32:esp32c3 # comment\n");
    write_file(root_sketch / "src" / "effects" / "fade.cpp", "void fade() {}\n");

    sheen::sketch_defaults disk_defaults = sheen::read_sketch_defaults(root_sketch.string());
    check(disk_defaults.fqbn == "esp32:esp32:esp32c3",
          "read_sketch_defaults falls back to sketch.yml on disk");

    write_file(root_sketch / "sketch.yaml", "default_fqbn: esp32:esp32:esp32da\n");
    sheen::sketch_defaults preferred_yaml = sheen::read_sketch_defaults(root_sketch.string());
    check(preferred_yaml.fqbn == "esp32:esp32:esp32da",
          "sketch.yaml takes precedence over sketch.yml when both exist");

    check(sheen::resolve_sketch_directory(root_sketch.string()) == root_sketch.string(),
          "resolve_sketch_directory keeps sketch root directly");
    check(sheen::resolve_sketch_directory((root_sketch / "Blink.ino").string()) == root_sketch.string(),
          "resolve_sketch_directory resolves main .ino file to sketch root");
    check(sheen::resolve_sketch_directory((root_sketch / "src" / "effects").string()) == root_sketch.string(),
          "resolve_sketch_directory climbs from nested src/effects subfolder to sketch root");

    std::filesystem::path monorepo = std::filesystem::path(temp_root) / "device-repo";
    write_file(monorepo / "README.md", "# Repo\n");
    write_file(monorepo / "build" / "stale.ino", "void ignored() {}\n");
    write_file(monorepo / "cached_out" / "build.options.json", "{}\n");
    write_file(monorepo / "cached_out" / "cached_out.ino", "void ignored() {}\n");

    write_file(monorepo / "a_loose" / "stray.ino", "void loose() {}\n");
    write_file(monorepo / "firmware" / "sensor_node" / "sensor_node.pde", "void setup() {}\n");
    check(sheen::resolve_sketch_directory(monorepo.string()) == (monorepo / "firmware" / "sensor_node").string(),
          "resolve_sketch_directory prefers strict <folder>/<folder>.pde over shallower loose .ino folder");
}

static void check_installed_library_scanner(const std::string& temp_root) {
    std::filesystem::path libs_dir = std::filesystem::path(temp_root) / "libraries";
    std::filesystem::path hw_dir = std::filesystem::path(temp_root) / "hardware";

    write_file(libs_dir / "FastLED" / "library.properties", "# comment line\nname = FastLED\nversion = 3.10.9\n");
    write_file(libs_dir / "UnnamedLib" / "library.properties", "version = 0.2.0\n");
    write_file(libs_dir / "LegacyOneWire" / "OneWire.h", "#pragma once\n");
    write_file(libs_dir / "BrokenFolder" / "README.txt", "no header or properties\n");
    write_file(libs_dir / ".staging" / "library.properties", "name = Ignored\nversion = 1.0.0\n");
    write_file(hw_dir / "esp32" / "esp32" / "3.3.12" / "platform.txt", "name=ESP32 Arduino\n");

    std::vector<sheen::installed_library> items =
        sheen::scan_installed_libraries(libs_dir.string(), hw_dir.string());
    check(items.size() == 4, "scanner finds properties libraries, legacy header library, and installed core");
    if (items.size() == 4) {
        check(items[0].name == "FastLED" && items[0].version == "3.10.9" && !items[0].is_core,
              "FastLED library name and version");
        check(items[1].name == "LegacyOneWire" && items[1].version.empty() && !items[1].is_core,
              "legacy header-only library");
        check(items[2].name == "UnnamedLib" && items[2].version == "0.2.0" && !items[2].is_core,
              "library without name= property falls back to folder name");
        check(items[3].name == "esp32-core" && items[3].version == "3.3.12" && items[3].is_core,
              "installed core sorts after user libraries");
    }
}

int main() {
    std::string temp_root = make_temp_dir();

    check_dependency_probes_and_fqbn();
    check_board_catalog_preservation();
    check_mcufw_events();
    check_memory_and_progress_formatting();
    if (!temp_root.empty()) {
        check_diagnostics_and_library_hints(temp_root);
        check_sketch_yaml_and_workspace_paths(temp_root);
        check_installed_library_scanner(temp_root);
        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    if (g_failures != 0) {
        std::printf("embedded_protocol: %llu of %llu checks failed\n",
                    static_cast<unsigned long long>(g_failures),
                    static_cast<unsigned long long>(g_checks));
        return 1;
    }

    std::printf("embedded_protocol: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
