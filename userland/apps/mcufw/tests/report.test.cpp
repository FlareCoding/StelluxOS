/* report, run on the build machine: the listing's aligned columns, a board's description with its choices wrapped,
 * a build's lines, the lines `--events` writes, and failures */

#include "build.hpp"
#include "report.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;
constexpr uint64_t BUILD_START_NS = 1'000'000'000;

static const sketch LAMP = { "/sketches/My Lamp", "My Lamp", "My Lamp.ino", {}, {}, {} };
static const build_result LAMP_RESULT = { "/b", "/b/My Lamp.ino.elf", "/b/My Lamp.ino.bin", false };

/* esp32da's partition schemes in Arduino-ESP32 3.3.12, enough to wrap a line at 120 columns */
constexpr const char* PARTITION_SCHEMES[] = {
    "default", "defaultffat", "default_8MB", "minimal", "no_fs", "no_ota", "noota_3g", "noota_ffat", "noota_3gffat",
    "huge_app", "min_spiffs", "fatflash", "app3M_fat9M_16MB", "rainmaker", "rainmaker_4MB", "rainmaker_8MB", "custom",
};

/* Nine choices of ten columns and one of four, which with the menu column fill a 120 column line exactly */
constexpr const char* EXACT_WIDTH_CHOICES[] = {
    "option_001", "option_002", "option_003", "option_004", "option_005", "option_006", "option_007", "option_008",
    "option_009", "tail",
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

struct capture {
    FILE* out;
    FILE* err;
};

static capture open_capture() {
    return { tmpfile(), tmpfile() };
}

static std::string read_back(FILE* file) {
    std::string text;
    char chunk[4096];
    rewind(file);
    while (size_t count = fread(chunk, 1, sizeof(chunk), file)) {
        text.append(chunk, count);
    }

    fclose(file);

    return text;
}

static installed_core esp32_core() {
    installed_core core;
    core.packager = "esp32";
    core.architecture = "esp32";
    core.version = "3.3.12";
    core.platform.set("name", "ESP32 Arduino");

    board_menu upload_speed = { "UploadSpeed", "Upload Speed", { { "921600", "921600" }, { "115200", "115200" } } };
    board_menu partition_scheme = { "PartitionScheme", "Partition Scheme", {} };
    for (const char* scheme : PARTITION_SCHEMES) {
        partition_scheme.choices.push_back({ scheme, scheme });
    }

    partition_scheme.choices[0].label = "Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)";
    core.boards.push_back({ "esp32da", "ESP32-WROOM-DA Module", false, {}, { upload_speed, partition_scheme } });

    board_menu exact = { "Exact", "Exact Width", {} };
    for (const char* choice : EXACT_WIDTH_CHOICES) {
        exact.choices.push_back({ choice, choice });
    }

    core.boards.back().menus.push_back(exact);

    return core;
}

static std::vector<listed_board> two_boards() {
    board_requirements missing;
    missing.missing_program = "xtensa-esp32s3-elf-g++";
    missing.missing_tools.push_back({ "esp32", "esp32s3-libs", "3.3.12" });
    return {
        { "esp32:esp32:esp32da", "ESP32-WROOM-DA Module", {} },
        { "esp32:esp32:esp32s3-octal", "ESP32S3 Dev Module Octal (WROOM2)", missing },
    };
}

static void check_listing() {
    capture person = open_capture();
    reporter(false, person.out, person.err).boards_listed(two_boards(), "");
    std::string expected =
        "esp32:esp32:esp32da         ESP32-WROOM-DA Module\n"
        "esp32:esp32:esp32s3-octal   ESP32S3 Dev Module Octal (WROOM2)   "
        "needs xtensa-esp32s3-elf-g++, esp32:esp32s3-libs 3.3.12\n";
    check(read_back(person.out) == expected, "a person sees aligned columns and what a board needs");
    check(read_back(person.err).empty(), "a listing writes nothing on stderr");

    capture program = open_capture();
    reporter(true, program.out, program.err).boards_listed(two_boards(), "");
    expected =
        "board\tesp32:esp32:esp32da\t-\tESP32-WROOM-DA Module\n"
        "board\tesp32:esp32:esp32s3-octal\txtensa-esp32s3-elf-g++, esp32:esp32s3-libs 3.3.12\t"
        "ESP32S3 Dev Module Octal (WROOM2)\n";
    check(read_back(program.out) == expected, "a program sees one line per board, `-` when it needs nothing");
    fclose(program.err);
}

static void check_empty_listing() {
    capture all = open_capture();
    reporter(false, all.out, all.err).boards_listed({}, "");
    check(read_back(all.out) == "No installed core defines a board. Install a core with mcufw install.\n",
          "no board at all says so");
    fclose(all.err);

    capture matching = open_capture();
    reporter(false, matching.out, matching.err).boards_listed({}, "wroom");
    check(read_back(matching.out) == "No installed board matches wroom.\n", "no match names the word");
    fclose(matching.err);

    capture program = open_capture();
    reporter(true, program.out, program.err).boards_listed({}, "wroom");
    check(read_back(program.out).empty(), "a program sees no lines");
    fclose(program.err);
}

static void check_description() {
    installed_core core = esp32_core();
    std::vector<option_setting> defaults = { { "UploadSpeed", "921600" }, { "PartitionScheme", "default" } };
    selected_board board = { &core, &core.boards[0], {}, defaults };
    board_requirements missing;
    missing.missing_tools.push_back({ "esp32", "esp32-libs", "3.3.12" });

    capture person = open_capture();
    reporter(false, person.out, person.err).board_described(board, missing);
    std::string expected =
        "esp32:esp32:esp32da   ESP32-WROOM-DA Module, from ESP32 Arduino 3.3.12\n"
        "needs esp32:esp32-libs 3.3.12\n"
        "\n"
        "UploadSpeed      921600 115200\n"
        "PartitionScheme  default defaultffat default_8MB minimal no_fs no_ota noota_3g noota_ffat noota_3gffat "
        "huge_app\n"
        "                 min_spiffs fatflash app3M_fat9M_16MB rainmaker rainmaker_4MB rainmaker_8MB custom\n"
        "Exact            option_001 option_002 option_003 option_004 option_005 option_006 option_007 option_008 "
        "option_009 tail\n";
    check(read_back(person.out) == expected, "choices wrap under the first choice within 120 columns");
    fclose(person.err);

    capture program = open_capture();
    reporter(true, program.out, program.err).board_described(board, {});
    std::string lines = read_back(program.out);
    fclose(program.err);

    std::string first_lines =
        "board\tesp32:esp32:esp32da:UploadSpeed=921600,PartitionScheme=default\t-\tESP32-WROOM-DA Module\n"
        "option\tUploadSpeed\t921600\t921600\n"
        "option\tUploadSpeed\t115200\t115200\n"
        "option\tPartitionScheme\tdefault\tDefault 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)\n";
    check(lines.starts_with(first_lines), "a program sees the full FQBN, then every choice with its label");
    check(std::count(lines.begin(), lines.end(), '\n') == 30, "one line per choice");
}

static selected_board default_board(const installed_core& core) {
    std::vector<option_setting> defaults = { { "UploadSpeed", "921600" }, { "PartitionScheme", "default" } };
    return { &core, &core.boards[0], {}, defaults };
}

static void check_build_lines() {
    installed_core core = esp32_core();
    selected_board board = default_board(core);
    capture person = open_capture();
    reporter shown(false, person.out, person.err);

    shown.sketch_found(LAMP);
    shown.board_selected(board, true);
    shown.core_compiling(core, "/b/core/core.a", 60);

    shown.progress("compile", 0, 75, BUILD_START_NS);
    shown.progress("compile", 40, 75, BUILD_START_NS + 200'000'000);
    shown.tool_output("compile", "My Lamp.ino:3:1: warning: unused");
    shown.progress("compile", 75, 75, BUILD_START_NS + 250'000'000);

    shown.size_checked({ 1'034'612, 44'932, true }, 1'310'720, 327'680);
    shown.built(LAMP_RESULT, 108'400'000'000);

    std::string expected =
        "Board      ESP32-WROOM-DA Module (esp32:esp32:esp32da), saved in sketch.yaml\n"
        "Core       ESP32 Arduino 3.3.12, 60 files\n"
        "Compiling  75 of 75\n"
        "Flash      1,034,612 of 1,310,720 bytes, 78%\n"
        "RAM        44,932 of 327,680 bytes, 13%\n"
        "Built      /b/My Lamp.ino.bin in 1 min 48 s\n";
    check(read_back(person.out) == expected,
          "a person sees labeled lines, and counts only at the end when stdout is no terminal");
    check(read_back(person.err) == "My Lamp.ino:3:1: warning: unused\n", "a tool's output goes to stderr whole");
}

static void check_rebuild_lines() {
    capture person = open_capture();
    reporter shown(false, person.out, person.err);
    shown.compiles_needed(1, 75, "My Lamp.ino changed");
    shown.progress("compile", 0, 1, BUILD_START_NS);
    shown.progress("compile", 1, 1, BUILD_START_NS + 50'000'000);
    shown.built(LAMP_RESULT, 4'100'000'000);

    build_result untouched = LAMP_RESULT;
    untouched.up_to_date = true;
    shown.built(untouched, 300'000'000);

    std::string expected =
        "Compiling  1 of 75, My Lamp.ino changed\n"
        "Built      /b/My Lamp.ino.bin in 4 s\n"
        "Built      /b/My Lamp.ino.bin is up to date\n";
    check(read_back(person.out) == expected,
          "a rebuild's last count gives the compiles it needs of all planned, with the first reason");
    fclose(person.err);

    capture program = open_capture();
    reporter events(true, program.out, program.err);
    events.compiles_needed(1, 75, "My Lamp.ino changed");
    events.progress("compile", 1, 1, BUILD_START_NS);
    check(read_back(program.out) == "progress\tcompile\t1\t1\n", "a program counts only the compiles that run");
    fclose(program.err);
}

static void check_cached_core() {
    capture person = open_capture();
    reporter(false, person.out, person.err).core_cached("/var/cache/mcufw/cores/esp32da-0123456789abcdef/core.a");
    check(read_back(person.out) == "Core       cached\n", "a person reads that the core came from the cache");
    fclose(person.err);

    capture program = open_capture();
    reporter(true, program.out, program.err).core_cached("/var/cache/mcufw/cores/esp32da-0123456789abcdef/core.a");
    check(read_back(program.out) == "core\tcached\t/var/cache/mcufw/cores/esp32da-0123456789abcdef/core.a\n",
          "a program reads the archive the build links");
    fclose(program.err);
}

static void check_build_events() {
    installed_core core = esp32_core();
    selected_board board = default_board(core);
    capture program = open_capture();
    reporter events(true, program.out, program.err);

    events.sketch_found(LAMP);
    events.board_selected(board, false);
    events.core_compiling(core, "/b/core/core.a", 60);

    events.progress("compile", 0, 75, BUILD_START_NS);
    events.progress("compile", 1, 75, BUILD_START_NS + 50'000'000);
    events.progress("compile", 2, 75, BUILD_START_NS + 100'000'000);
    events.tool_output("compile", "a.ino:1:2: error: x\r\n  1 | y\n");
    events.problem({ problem_severity::error, "/sketches/My Lamp/a.ino", 1, 2, "x" });
    events.progress("compile", 75, 75, BUILD_START_NS + 120'000'000);

    events.size_checked({ 100, 0, false }, 0, 0);
    events.built(LAMP_RESULT, 1);

    std::string expected =
        "sketch\tMy Lamp\t/sketches/My Lamp\n"
        "board\tesp32:esp32:esp32da:UploadSpeed=921600,PartitionScheme=default\tESP32-WROOM-DA Module\n"
        "core\tcompile\t/b/core/core.a\n"
        "progress\tcompile\t0\t75\n"
        "progress\tcompile\t2\t75\n"
        "log\tcompile\ta.ino:1:2: error: x\n"
        "log\tcompile\t  1 | y\n"
        "problem\terror\t/sketches/My Lamp/a.ino\t1\t2\tx\n"
        "progress\tcompile\t75\t75\n"
        "size\tflash\t100\t-\n"
        "built\t/b/My Lamp.ino.bin\n";
    check(read_back(program.out) == expected, "a program sees each fact, counts at most every 100 ms and at the end");
    check(read_back(program.err).empty(), "a program's build writes nothing on stderr");
}

static void check_commands() {
    capture person = open_capture();
    reporter shown(false, person.out, person.err);
    shown.command_started({ "g++", "-DBOARD=\"LAMP\"", "/my sketch/a.cpp", "-o", "" });
    check(read_back(person.out) == "g++ \"-DBOARD=\\\"LAMP\\\"\" \"/my sketch/a.cpp\" -o \n",
          "a command's words are quoted and escaped as arduino-cli prints them");
    fclose(person.err);

    capture tabbed = open_capture();
    reporter(false, tabbed.out, tabbed.err).command_started({ "g++", "a\tb" });
    check(read_back(tabbed.out) == "g++ \"a\tb\"\n", "a word holding a tab is quoted");
    fclose(tabbed.err);

    capture program = open_capture();
    reporter(true, program.out, program.err).command_started({ "g++" });
    check(read_back(program.out).empty(), "a program sees no commands");
    fclose(program.err);
}

static void check_library_lines() {
    library wifi = {};
    wifi.name = "WiFi";
    wifi.version = "3.3.12";
    wifi.directory = "/cores/esp32/libraries/WiFi";

    library servo = wifi;
    servo.name = "Servo";
    servo.version = "";
    servo.architectures = { "avr", "megaavr" };

    capture person = open_capture();
    reporter shown(false, person.out, person.err);
    shown.progress("libraries", 0, 1, BUILD_START_NS);
    shown.progress("libraries", 3, 3, BUILD_START_NS + 1);

    shown.libraries_found({ &wifi, &servo });
    shown.library_suggested("FastLED.h", { "FastLED", "3.10.9" });
    shown.library_incompatible(servo, { "esp32", "xtensa" });
    check(read_back(person.out) == "Libraries  WiFi 3.3.12, Servo\n", "the list replaces the final count");
    check(read_back(person.err) == "FastLED 3.10.9 provides FastLED.h: mcufw install FastLED\n"
                                   "WARNING: library Servo claims to run on avr, megaavr architecture(s) and may be "
                                   "incompatible with your current board which runs on esp32, xtensa "
                                   "architecture(s).\n",
          "the hint names its command, and an incompatible library is warned about in arduino-cli's words");

    capture spaced = open_capture();
    reporter(false, spaced.out, spaced.err).library_suggested("Adafruit_SSD1306.h", { "Adafruit SSD1306", "2.5.17" });
    check(read_back(spaced.err) ==
              "Adafruit SSD1306 2.5.17 provides Adafruit_SSD1306.h: mcufw install \"Adafruit SSD1306\"\n",
          "a library name with a space is quoted in its command");
    fclose(spaced.out);

    capture program = open_capture();
    reporter events(true, program.out, program.err);
    events.progress("libraries", 3, 3, BUILD_START_NS);
    events.libraries_found({ &wifi });
    events.library_suggested("FastLED.h", { "FastLED", "3.10.9" });

    std::string expected =
        "progress\tlibraries\t3\t3\n"
        "library\tWiFi\t3.3.12\t/cores/esp32/libraries/WiFi\n"
        "hint\tFastLED.h\tFastLED\t3.10.9\n";
    check(read_back(program.out) == expected, "a program sees each library and the hint");
    fclose(program.err);
}

static void check_failures() {
    capture person = open_capture();
    reporter(false, person.out, person.err).failed({ failure_kind::board, "no installed core defines board x" });
    check(read_back(person.out).empty(), "a person's failure leaves stdout alone");
    check(read_back(person.err) == "mcufw: no installed core defines board x\n", "a person's failure goes to stderr");

    capture program = open_capture();
    reporter(true, program.out, program.err).failed({ failure_kind::cancelled, "stopped\tby a\nsignal" });
    check(read_back(program.out) == "failed\tcancelled\tstopped by a signal\n",
          "a field's tabs and newlines are spaces");
    check(read_back(program.err).empty(), "a program's failure leaves stderr alone");
}

// On a terminal, a failure clears the in-place compile count before its reason, so the reason starts a clean line
static void check_failure_on_terminal() {
    int controller = posix_openpt(O_RDWR | O_NOCTTY);
    if (controller < 0 || grantpt(controller) != 0 || unlockpt(controller) != 0) {
        check(false, "a pseudo-terminal opens");
        return;
    }

    FILE* terminal = fopen(ptsname(controller), "w");
    capture person = open_capture();
    if (terminal == nullptr) {
        check(false, "the pseudo-terminal's other end opens");
        return;
    }

    reporter shown(false, terminal, person.err);
    shown.progress("compile", 1, 10, BUILD_START_NS);
    shown.failed({ failure_kind::compile, "1 error in Lamp.ino" });
    fflush(terminal);

    char buffer[256] = {};
    ssize_t length = read(controller, buffer, sizeof(buffer) - 1);
    fclose(terminal);

    std::string shown_text = length > 0 ? std::string(buffer, static_cast<size_t>(length)) : "";
    check(shown_text.find("Compiling  1 of 10") != std::string::npos && shown_text.ends_with("\r\x1b[K"),
          "the count is cleared before the failure: " + shown_text);
    check(read_back(person.err) == "mcufw: 1 error in Lamp.ino\n", "and the reason goes to stderr");
    close(controller);
}

static void check_installs() {
    capture program = open_capture();
    reporter report(true, program.out, program.err);
    report.install_started("esp32:esp32", "3.3.12", 1000);
    report.install_progress("esp32:esp32", 100, 1000, 1'000'000'000);
    report.install_progress("esp32:esp32", 200, 1000, 1'050'000'000);
    report.install_progress("esp32:esp32", 1000, 1000, 1'060'000'000);
    report.installed("esp32:esp32", "3.3.12", false);
    report.removed("esp32:esp32-libs", "3.3.11");
    report.program_missing({ "esp32", "esp-x32", "2601" });
    report.program_missing({ "", "xtensa-esp32s3-elf-g++", "" });

    std::string expected =
        "start\tesp32:esp32\t3.3.12\t1000\n"
        "progress\tesp32:esp32\t100\t1000\n"
        "progress\tesp32:esp32\t1000\t1000\n"
        "installed\tesp32:esp32\t3.3.12\n"
        "removed\tesp32:esp32-libs\t3.3.11\n"
        "needs\tesp32:esp-x32\t2601\n"
        "needs\txtensa-esp32s3-elf-g++\t-\n";
    check(read_back(program.out) == expected, "progress at most every 100 ms and at the end");
    fclose(program.err);

    capture person = open_capture();
    reporter sentences(false, person.out, person.err);
    sentences.install_started("esp32:esp32", "3.3.12", 51102895);
    sentences.installed("esp32:esp32", "3.3.12", false);
    sentences.installed("esp32:esp32-libs", "3.3.12", true);
    sentences.removed("esp32:esp32-libs", "3.3.11");
    sentences.program_missing({ "esp32", "esp-x32", "2601" });
    sentences.program_missing({ "", "xtensa-esp32s3-elf-g++", "" });
    expected =
        "Installing esp32:esp32 3.3.12, 51.1 MB\n"
        "Installed esp32:esp32 3.3.12\n"
        "esp32:esp32-libs 3.3.12 is installed already\n"
        "Removed esp32:esp32-libs 3.3.11\n"
        "The board needs esp32:esp-x32 2601, a program only a Stellux package installs.\n"
        "The board needs xtensa-esp32s3-elf-g++, a program only a Stellux package installs.\n";
    check(read_back(person.out) == expected, "a person without a terminal sees whole lines");
    fclose(person.err);
}

static void check_warnings() {
    capture person = open_capture();
    reporter(false, person.out, person.err).warned("cannot download the library index");
    check(read_back(person.out).empty(), "a person's warning leaves stdout alone");
    check(read_back(person.err) == "mcufw: cannot download the library index\n", "a person's warning goes to stderr");

    capture program = open_capture();
    reporter(true, program.out, program.err).warned("cannot download the library index");
    check(read_back(program.out) == "warning\tcannot download the library index\n", "a program sees a warning event");
    fclose(program.err);
}

int main() {
    check_listing();
    check_empty_listing();
    check_description();
    check_failures();
    check_failure_on_terminal();

    check_installs();
    check_warnings();

    check_build_lines();
    check_rebuild_lines();
    check_cached_core();
    check_build_events();
    check_commands();
    check_library_lines();

    if (g_failures != 0) {
        printf("report: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("report: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
