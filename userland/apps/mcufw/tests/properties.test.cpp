/* properties, run on the build machine: Arduino's line rules, the Linux keys, a key set again moving to the end,
 * subtrees, and placeholder expansion with its pass limit */

#include "properties.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

/* boards.txt:2146-2159 of Arduino-ESP32 3.3.12, which give four upload speeds as read on Linux */
constexpr const char* UPLOAD_SPEED_LINES =
    "esp32da.menu.UploadSpeed.921600=921600\n"
    "esp32da.menu.UploadSpeed.921600.upload.speed=921600\n"
    "esp32da.menu.UploadSpeed.115200=115200\n"
    "esp32da.menu.UploadSpeed.115200.upload.speed=115200\n"
    "esp32da.menu.UploadSpeed.256000.windows=256000\n"
    "esp32da.menu.UploadSpeed.256000.upload.speed=256000\n"
    "esp32da.menu.UploadSpeed.230400.windows.upload.speed=256000\n"
    "esp32da.menu.UploadSpeed.230400=230400\n"
    "esp32da.menu.UploadSpeed.230400.upload.speed=230400\n"
    "esp32da.menu.UploadSpeed.460800.linux=460800\n"
    "esp32da.menu.UploadSpeed.460800.macosx=460800\n"
    "esp32da.menu.UploadSpeed.460800.upload.speed=460800\n"
    "esp32da.menu.UploadSpeed.512000.windows=512000\n"
    "esp32da.menu.UploadSpeed.512000.upload.speed=512000\n";

/* The lines that find the C++ compiler for esp32da: platform.txt's, boards.txt's without the board prefix, and the
 * toolchain link's path */
constexpr const char* COMPILER_LINES =
    "compiler.path={tools.{build.tarch}-esp-elf-gcc.path}/bin/\n"
    "tools.xtensa-esp-elf-gcc.path={runtime.tools.esp-x32.path}\n"
    "compiler.prefix={build.tarch}-{build.target}-elf-\n"
    "compiler.cpp.cmd={compiler.prefix}g++\n"
    "build.tarch=xtensa\n"
    "build.target=esp32\n"
    "runtime.tools.esp-x32.path=/usr/arduino/tools/esp32/esp-x32/2601\n";

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

static property_table parsed(const std::string& text) {
    property_table table;
    std::string error;
    check(parse_properties(text, table, error), "the text parses");
    return table;
}

static std::string joined(const std::vector<std::string>& words) {
    std::string text;
    for (const std::string& word : words) {
        text += text.empty() ? "" : " ";
        text += word;
    }

    return text;
}

static void check_lines() {
    property_table table = parsed("# a comment\n\n   name = ESP32 Arduino  \nflags=-DA=1 -DB=2\r\nempty=\rlast=x");
    check(table.get("name") == "ESP32 Arduino", "keys and values are trimmed");
    check(table.get("flags") == "-DA=1 -DB=2", "a value keeps every `=` after the first");
    check(table.find("empty") && table.get("empty").empty(), "an empty value is set");
    check(table.get("last") == "x", "CR LF and CR end lines as LF does");
    check(joined(table.keys()) == "name flags empty last", "keys keep their file order");

    property_table refused;
    std::string error;
    check(!parse_properties("good=1\nno equals here\n", refused, error), "a line without `=` fails the text");
    check(error.find("line 2") != std::string::npos, "the failure names the line");
}

static void check_linux_keys() {
    property_table table = parsed("cmd=esptool\ncmd.linux=esptool.py\ncmd.windows=esptool.exe\ntools.linux.path=x\n");
    check(table.get("cmd") == "esptool.py", "a `.linux` key replaces the plain key read before it");
    check(table.get("cmd.windows") == "esptool.exe", "another system's key stays a key of its own");
    check(table.get("tools.path") == "x", "the first `.linux` is dropped wherever it stands");

    property_table speeds = parsed(UPLOAD_SPEED_LINES).subtree("esp32da.menu.UploadSpeed");
    check(joined(speeds.first_level_keys()) == "921600 115200 256000 230400 460800 512000",
          "first-level keys keep their order of first appearance");
    check(speeds.get("460800") == "460800", "Linux's 460800 baud is a labeled choice");
    check(!speeds.find("256000") && !speeds.find("512000"), "Windows' 256000 and 512000 carry no label");
}

static void check_order() {
    property_table table = parsed("a=1\nb=2\na=3\n");
    check(joined(table.keys()) == "b a", "setting a key again moves it to the end");
    check(table.get("a") == "3", "the later value wins");

    property_table overrides = parsed("b=4\nc=5\n");
    table.merge(overrides);
    check(joined(table.keys()) == "a b c", "a merge sets each key in the other table's order");
    check(table.get("b") == "4", "the merged value wins");
}

static void check_encoding() {
    property_table latin1 = parsed(std::string("name=Caf\xE9 board\n"));
    check(latin1.get("name") == "Caf\xC3\xA9 board", "text that is not UTF-8 reads as ISO 8859-1");

    property_table utf8 = parsed("name=Caf\xC3\xA9 board\n");
    check(utf8.get("name") == "Caf\xC3\xA9 board", "UTF-8 text stays as it is");

    property_table overlong = parsed(std::string("name=\xC0\xAF\n"));
    check(overlong.get("name") == "\xC3\x80\xC2\xAF", "an overlong form is not UTF-8");

    property_table surrogate = parsed(std::string("name=\xED\xA0\x80\n"));
    check(surrogate.get("name") == "\xC3\xAD\xC2\xA0\xC2\x80", "a surrogate's encoding is not UTF-8");

    property_table long_overlong = parsed(std::string("name=\xE0\x80\xAF\n"));
    check(long_overlong.get("name") == "\xC3\xA0\xC2\x80\xC2\xAF", "a three-byte overlong form is not UTF-8");

    property_table past_unicode = parsed(std::string("name=\xF4\x90\x80\x80\n"));
    check(past_unicode.get("name") == "\xC3\xB4\xC2\x90\xC2\x80\xC2\x80", "a code point past U+10FFFF is not UTF-8");
}

static void check_trees() {
    property_table table = parsed("menu.CPU=CPU\nuno.name=Uno\nuno.upload.tool=avrdude\nversion=1\nmega.name=Mega\n");
    check(joined(table.first_level_keys()) == "menu uno version mega", "a key without a dot is a first level key");

    std::vector<std::pair<std::string, property_table>> split = table.split_first_level();
    check(split.size() == 3, "keys without a dot head no subtree");
    check(split[1].first == "uno" && split[1].second.get("upload.tool") == "avrdude", "a subtree drops its head");
    check(table.subtree("uno").get("name") == "Uno", "subtree drops the prefix and its dot");
    check(!table.subtree("un").find("o.name"), "subtree matches whole parts only");
}

static void check_expansion() {
    property_table compiler = parsed(COMPILER_LINES);
    check(compiler.expand("{compiler.path}{compiler.cpp.cmd}") ==
              "/usr/arduino/tools/esp32/esp-x32/2601/bin/xtensa-esp32-elf-g++",
          "nested placeholders expand through every level");

    property_table table = parsed("known=value\ninner=in\n");
    check(table.expand("{unknown} {known}") == "{unknown} value", "a placeholder naming no key stays");
    check(table.expand("{outer.{inner}} {}") == "{outer.in} {}",
          "an unknown outer placeholder stays, its inner one expanded");
    check(table.expand("no placeholders") == "no placeholders", "text without placeholders stays");

    property_table looping = parsed("a={a}x\n");
    check(looping.expand("{a}") == "{a}xxxxxxxxxx", "expansion stops after its pass limit");
}

int main() {
    check_lines();
    check_linux_keys();
    check_order();
    check_encoding();
    check_trees();
    check_expansion();

    if (g_failures != 0) {
        printf("properties: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("properties: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
