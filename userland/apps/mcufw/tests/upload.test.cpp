/* upload_words, the touch and wait keys, and last_percentage, run on the build machine: esp32da's upload with the
 * Stellux overrides, the tool and keys arduino-cli picks, and the progress esptool and avrdude print */

#include "upload.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

static const char* const BUILD = "/var/cache/mcufw/builds/WiFiScan-3f9c2a17";
static const char* const PORT  = "/dev/serial/cp2102-0001";

// esp32da's build properties as far as its upload reads them, from Arduino-ESP32 3.3.12 and its Stellux overrides
static const char* const ESP32DA_UPLOAD = R"(
runtime.platform.path=/var/lib/mcufw/hardware/esp32/esp32/3.3.12
tools.esptool_py.path={runtime.platform.path}/tools/esptool
tools.esptool_py.cmd=esptool
tools.esptool_py.upload.protocol=serial
tools.esptool_py.upload.params.verbose=
tools.esptool_py.upload.params.quiet=
tools.esptool_py.upload.pattern_args=--chip {build.mcu} --port "{serial.port}" --baud {upload.speed} {upload.flags} --before default-reset --after hard-reset write-flash {upload.erase_cmd} -z --flash-mode keep --flash-freq keep --flash-size keep {build.bootloader_addr} "{build.path}/{build.project_name}.bootloader.bin" 0x8000 "{build.path}/{build.project_name}.partitions.bin" 0xe000 "{runtime.platform.path}/tools/partitions/boot_app0.bin" 0x10000 "{build.path}/{build.project_name}.bin" {upload.extra_flags}
tools.esptool_py.upload.pattern=python3 -m esptool {upload.pattern_args}
upload.tool=esptool_py
upload.tool.default=esptool_py
upload.tool.network=esp_ota
upload.flags=
upload.erase_cmd=
upload.extra_flags=
upload.speed=921600
upload.use_1200bps_touch=false
upload.wait_for_upload_port=false
build.mcu=esp32
build.bootloader_addr=0x1000
build.path=/var/cache/mcufw/builds/WiFiScan-3f9c2a17
build.project_name=WiFiScan.ino
)";

// An Uno's upload as ArduinoCore-avr writes it, whose recipe names the verbosity and verification keys
static const char* const UNO_UPLOAD = R"(
tools.avrdude.cmd.path=/usr/bin/avrdude
tools.avrdude.config.path=/etc/avrdude.conf
tools.avrdude.upload.params.verbose=-v -v -v -v
tools.avrdude.upload.params.quiet=-q -q
tools.avrdude.upload.params.noverify=-V
tools.avrdude.upload.pattern="{cmd.path}" "-C{config.path}" {upload.verbose} {upload.verify} -p{build.mcu} -c{upload.protocol} "-P{serial.port}" -b{upload.speed} -D "-Uflash:w:{build.path}/{build.project_name}.hex:i"
upload.tool=avrdude
upload.protocol=arduino
upload.speed=115200
build.mcu=atmega328p
build.path=/var/cache/mcufw/builds/Blink-00000000
build.project_name=Blink.ino
)";

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

// `text`, then each `key=value` of `changes` over it
static property_table properties_of(const char* text, const char* changes = "") {
    property_table properties;
    std::string error;
    if (!parse_properties(text, properties, error) || !parse_properties(changes, properties, error)) {
        printf("  %s\n", error.c_str());
    }

    return properties;
}

static void check_esp32da() {
    std::string build = BUILD;
    std::vector<std::string> expected = {
        "python3", "-m", "esptool", "--chip", "esp32", "--port", PORT, "--baud", "921600", "--before",
        "default-reset", "--after", "hard-reset", "write-flash", "-z", "--flash-mode", "keep", "--flash-freq", "keep",
        "--flash-size", "keep", "0x1000", build + "/WiFiScan.ino.bootloader.bin", "0x8000",
        build + "/WiFiScan.ino.partitions.bin", "0xe000",
        "/var/lib/mcufw/hardware/esp32/esp32/3.3.12/tools/partitions/boot_app0.bin", "0x10000",
        build + "/WiFiScan.ino.bin",
    };

    std::vector<std::string> words;
    std::string error;
    check(upload_words(properties_of(ESP32DA_UPLOAD), PORT, false, words, error) && words == expected,
          "runs esptool with esp32da's arguments, the empty ones dropped");
    check(upload_words(properties_of(ESP32DA_UPLOAD, "upload.serial.speed=460800"), PORT, false, words, error) &&
              words.size() > 8 && words[8] == "460800",
          "takes a serial upload key over its plain key");
    check(!wants_1200_baud_touch(properties_of(ESP32DA_UPLOAD)) &&
              !waits_for_upload_port(properties_of(ESP32DA_UPLOAD)),
          "neither touches nor waits for esp32da");
}

static void check_tools() {
    const char* app_only = R"(
upload.tool.serial=esptool_py_app_only
tools.esptool_py_app_only.upload.protocol=serial
tools.esptool_py_app_only.upload.pattern=app-only {serial.port.file}
)";
    std::vector<std::string> words;
    std::string error;
    check(upload_words(properties_of(ESP32DA_UPLOAD, app_only), PORT, false, words, error) &&
              words == std::vector<std::string>({ "app-only", "serial/cp2102-0001" }),
          "prefers the serial tool and names the port without /dev/");

    std::vector<std::string> avrdude = {
        "/usr/bin/avrdude", "-C/etc/avrdude.conf", "-q", "-q", "-V", "-patmega328p", "-carduino",
        "-P/dev/serial/ch34x-0001", "-b115200", "-D",
        "-Uflash:w:/var/cache/mcufw/builds/Blink-00000000/Blink.ino.hex:i",
    };

    check(upload_words(properties_of(UNO_UPLOAD), "/dev/serial/ch34x-0001", false, words, error) && words == avrdude,
          "reads a plain upload.tool with the quiet and no-verify parameters");
    check(upload_words(properties_of(UNO_UPLOAD), "/dev/serial/ch34x-0001", true, words, error) && words.size() > 6 &&
              words[2] == "-v" && words[5] == "-v" && words[6] == "-V",
          "takes the verbose parameters when asked");
    check(upload_words(properties_of(UNO_UPLOAD, "tools.avrdude.upload.pattern= "), PORT, false, words, error) &&
              words.empty(),
          "uploads nothing for an empty pattern, as arduino-cli does");

    const char* touch = "upload.use_1200bps_touch= true \ntools.avrdude.upload.wait_for_upload_port=true\n";
    check(wants_1200_baud_touch(properties_of(UNO_UPLOAD, touch)) &&
              waits_for_upload_port(properties_of(UNO_UPLOAD, touch)),
          "reads the touch and the wait from the board and the tool's keys");
    check(!wants_1200_baud_touch(properties_of(UNO_UPLOAD, "upload.use_1200bps_touch=TRUE")),
          "reads only the word true as true");
    check(wants_1200_baud_touch(properties_of(UNO_UPLOAD, "upload.use_1200bps_touch=true")) &&
              !waits_for_upload_port(properties_of(UNO_UPLOAD, "upload.use_1200bps_touch=true")),
          "touches a board that sets only the touch without waiting for its port");
    check(waits_for_upload_port(properties_of(UNO_UPLOAD, "upload.wait_for_upload_port=true")) &&
              !wants_1200_baud_touch(properties_of(UNO_UPLOAD, "upload.wait_for_upload_port=true")),
          "waits for the port of a board that sets only the wait without touching it");

    const char* default_tool = "upload.tool.default=standin\ntools.standin.upload.pattern=standin {serial.port}\n";
    check(upload_words(properties_of(UNO_UPLOAD, default_tool), "/dev/serial/ch34x-0001", false, words, error) &&
              words == std::vector<std::string>({ "standin", "/dev/serial/ch34x-0001" }),
          "takes upload.tool.default over a plain upload.tool");
}

static void check_refusals() {
    std::vector<std::string> words;
    std::string error;
    check(!upload_words(properties_of(ESP32DA_UPLOAD, "upload.tool=\nupload.tool.default=\n"), PORT, false, words,
                        error) &&
              error == "the board names no upload tool",
          "refuses a board with no upload tool");
    check(!upload_words(properties_of(UNO_UPLOAD, "upload.tool=arduino:avrdude"), PORT, false, words, error) &&
              error.find("another platform") != std::string::npos,
          "refuses a tool from another platform");
    check(!upload_words(properties_of(UNO_UPLOAD, "tools.avrdude.upload.pattern=avrdude \"-P{serial.port}"), PORT,
                        false, words, error),
          "fails on a quote that does not close");

    property_table uno = properties_of(UNO_UPLOAD);
    property_table without_protocol;
    for (const std::string& key : uno.keys()) {
        if (key != "upload.protocol") {
            without_protocol.set(key, uno.get(key));
        }
    }

    check(!upload_words(without_protocol, PORT, false, words, error) &&
              error.find("needs a programmer") != std::string::npos,
          "refuses an upload that needs a programmer");
}

static void check_percentages() {
    check(last_percentage("Writing at 0x0001c000 [=>                            ]  12.5% 98304/786432 bytes... ") == 12,
          "reads esptool's percentage");
    check(last_percentage("\rWriting at 0x000ce000 [==============================] 100.0% 786432/786432 bytes... ") ==
              100,
          "reads esptool's last line after its carriage return");
    check(last_percentage("Writing | ################################################## | 100% 0.25s") == 100,
          "reads avrdude's percentage");

    check(last_percentage("50% done, 75% done") == 75, "takes the last percentage of a line");
    check(last_percentage("Hash of data verified.") == -1, "finds none in a line without one");
    check(last_percentage("% .5% 50%%") == 50, "skips signs with no number before them");
    check(last_percentage("99999999999%") == -1, "skips a number too large to be one");
}

int main() {
    check_esp32da();
    check_tools();
    check_refusals();
    check_percentages();

    if (g_failures != 0) {
        printf("upload: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("upload: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
