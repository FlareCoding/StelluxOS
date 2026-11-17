/* sizes, run on the build machine: `size -A` output summed by Arduino-ESP32 3.3.12's own regexes, a core without a
 * data regex, the limits read as strconv.Atoi reads them, and checkSize's verdicts at each limit */

#include "sizes.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

constexpr const char* FLASH_REGEX = "^(?:\\.iram0\\.text|\\.iram0\\.vectors|\\.dram0\\.data|\\.dram1\\.data|"
                                    "\\.flash\\.text|\\.flash\\.rodata|\\.flash\\.appdesc|\\.flash\\.init_array|"
                                    "\\.eh_frame|)\\s+([0-9]+).*";
constexpr const char* RAM_REGEX = "^(?:\\.dram0\\.data|\\.dram0\\.bss|\\.dram1\\.data|\\.dram1\\.bss|\\.noinit)"
                                  "\\s+([0-9]+).*";

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

static void check_esp32_output() {
    std::string output = "WiFiScan.ino.elf  :\n"
                         "section                     size         addr\n"
                         ".rtc.text                      0   1074528256\n"
                         ".iram0.vectors              1027   1074266112\n"
                         ".iram0.text                65251   1074267140\n"
                         ".dram0.data                16848   1073470304\n"
                         ".dram0.bss                 28084   1073487152\n"
                         ".noinit                        0   1073515236\n"
                         ".flash.appdesc               256   1061158944\n"
                         ".flash.rodata             174924   1061159200\n"
                         ".flash.text               776306   1074593824\n"
                         ".debug_info              1234567            0\n"
                         "Total                    2297263\n";

    program_size size = {};
    std::string error;
    check(sum_program_size(output, FLASH_REGEX, RAM_REGEX, "", size, error), "the ESP32 regexes compile");
    check(size.flash == 1027 + 65251 + 16848 + 256 + 174924 + 776306, "flash sums the sections its regex names");
    check(size.has_ram && size.ram == 16848 + 28084 + 0, "RAM sums the data and bss sections");
}

static void check_other_cores() {
    program_size size = {};
    std::string error;
    check(sum_program_size(".text  100\n.data  20\n", "^(?:\\.text|\\.data)\\s+([0-9]+).*", "", "", size, error),
          "a core without a data regex sums flash");
    check(size.flash == 120 && !size.has_ram, "and states no RAM");

    check(!sum_program_size(".text 1\n", "", "", "", size, error), "no flash regex fails");
    check(!sum_program_size(".text 1\n", "(unclosed", "", "", size, error), "a regex that does not compile fails");
    check(error.find("recipe.size.regex") != std::string::npos, "the failure names the key");
    check(!sum_program_size(".text 1\n", "^\\.text\\s+([0-9]+)", "", "(unclosed", size, error) &&
              error.find("recipe.size.regex.eeprom") != std::string::npos,
          "an eeprom regex that does not compile fails, as arduino-cli compiles it too");

    check(sum_program_size("12\n30\n", "^[0-9]+$", "", "", size, error) && size.flash == 42,
          "a whole match that is a number counts");
    check(sum_program_size("12\n30\n", "^([0-9]+)$", "", "", size, error) && size.flash == 84,
          "and its group counts again, as arduino-cli adds every submatch");
    check(sum_program_size("+5\n123456789012345678901\n", "^([+0-9]+)$", "", "", size, error) && size.flash == 10,
          "a signed number counts and one past 64 bits counts nothing, as strconv.Atoi reads them");
}

static void check_limits() {
    uint64_t flash_max = 1;
    uint64_t ram_max = 1;
    std::string error;

    check(read_size_limits("16000000", "", flash_max, ram_max, error) && flash_max == 16000000 && ram_max == 0,
          "a limit reads as a number, and an unset one as none");
    check(read_size_limits("+1310720", "-1", flash_max, ram_max, error) && flash_max == 1310720 && ram_max == 0,
          "a sign is allowed, and a negative limit is none");
    check(!read_size_limits("lots", "", flash_max, ram_max, error) &&
              error == "the board's upload.maximum_size, lots, is not a number",
          "a limit that is no number fails naming its key");
    check(!read_size_limits("1", "99999999999999999999", flash_max, ram_max, error) &&
              error.find("upload.maximum_data_size") != std::string::npos,
          "and so does one past 64 bits");
}

static void check_verdicts() {
    program_size size = { 1000, 750, true };
    check(check_program_size(size, 1000, 1000, "").failure.empty(), "a sketch exactly at the flash limit fits");
    check(check_program_size(size, 1000, 1000, "").warnings.empty(), "and RAM at exactly 75% gives no warning");

    size.flash = 1001;
    check(!check_program_size(size, 1000, 1000, "").failure.empty(), "one byte past the flash limit fails");
    check(check_program_size(size, 0, 0, "").failure.empty(), "a board without limits checks nothing");

    size = { 10, 751, true };
    check(check_program_size(size, 1000, 1000, "").warnings ==
              std::vector<std::string>{ "Low memory available, stability problems may occur." },
          "RAM one byte past 75% warns");
    check(check_program_size(size, 1000, 1000, "80").warnings.empty(), "the board's own share replaces 75%");
    check(check_program_size(size, 1000, 1000, "x").warnings ==
              std::vector<std::string>{ "Invalid value for build.warn_data_percentage: x",
                                        "Low memory available, stability problems may occur." },
          "an invalid share is warned about and 75% kept");

    size = { 10, 1001, true };
    check(!check_program_size(size, 1000, 1000, "").failure.empty(), "RAM past its limit fails");
    size_verdict unlimited = check_program_size(size, 1000, 0, "");
    check(unlimited.failure.empty() && unlimited.warnings.empty(), "a board without a RAM limit checks no RAM");

    size.has_ram = false;
    check(check_program_size(size, 1000, 1000, "").failure.empty(), "a core without a data regex checks no RAM");
}

int main() {
    check_esp32_output();
    check_other_cores();
    check_limits();
    check_verdicts();

    if (g_failures != 0) {
        printf("sizes: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("sizes: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
