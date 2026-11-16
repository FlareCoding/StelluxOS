/* report, run on the build machine: the listing's aligned columns, a board's description with its choices wrapped,
 * the lines `--events` writes, and failures */

#include "report.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

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

int main() {
    check_listing();
    check_empty_listing();
    check_description();
    check_failures();
    check_installs();

    if (g_failures != 0) {
        printf("report: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("report: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
