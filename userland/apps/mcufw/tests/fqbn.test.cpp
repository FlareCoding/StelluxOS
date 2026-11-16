/* fqbn, run on the build machine: board IDs and FQBNs with and without options, options trimmed and named again, the
 * names that fail, and the FQBN written back */

#include "fqbn.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

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

static bool has_options(const board_request& request, const std::vector<option_setting>& expected) {
    if (request.options.size() != expected.size()) {
        return false;
    }

    for (size_t i = 0; i < expected.size(); i++) {
        const option_setting& option = request.options[i];
        if (option.menu_id != expected[i].menu_id || option.choice_id != expected[i].choice_id) {
            return false;
        }
    }

    return true;
}

static void check_board_ids() {
    board_request request;
    std::string error;
    check(parse_board_request("esp32da", request, error), "a board ID parses");
    check(request.packager.empty() && request.architecture.empty(), "an ID alone names no core");
    check(request.board_id == "esp32da" && request.options.empty(), "an ID alone names the board");

    check(parse_board_request("esp32da:FlashSize=8M,PSRAM=enabled", request, error), "an ID with options parses");
    check(request.board_id == "esp32da", "the ID comes before the options");
    check(has_options(request, { { "FlashSize", "8M" }, { "PSRAM", "enabled" } }), "options keep their order");
}

static void check_fqbns() {
    board_request request;
    std::string error;
    check(parse_board_request("esp32:esp32:esp32da", request, error), "an FQBN parses");
    check(request.packager == "esp32" && request.architecture == "esp32", "an FQBN names the core");
    check(request.board_id == "esp32da" && request.options.empty(), "an FQBN names the board");

    check(parse_board_request("esp32:esp32:esp32da:UploadSpeed=115200", request, error), "an FQBN with options parses");
    check(has_options(request, { { "UploadSpeed", "115200" } }), "an FQBN carries its options");
}

static void check_options() {
    board_request request;
    std::string error;
    check(parse_board_request("esp32da: FlashSize = 8M ,PSRAM=enabled", request, error), "spaced options parse");
    check(has_options(request, { { "FlashSize", "8M" }, { "PSRAM", "enabled" } }),
          "option names and choices are trimmed");

    check(parse_board_request("esp32da:FlashSize=4M,PSRAM=enabled,FlashSize=8M", request, error),
          "a menu named twice parses");
    check(has_options(request, { { "PSRAM", "enabled" }, { "FlashSize", "8M" } }),
          "a menu named again takes its last choice and moves last");
}

static void check_failures() {
    const char* malformed[] = {
        "",
        "esp32:esp32",
        "esp32::esp32da",
        "esp32:esp32:esp32da:FlashSize=8M:x",
        "esp32:esp32:esp32da:FlashSize",
        "esp32:esp32:esp32da:FlashSize=",
        "esp32:esp32:esp32da:=8M",
        "esp32:esp32:esp32da:FlashSize=8M,,PSRAM=enabled",
        "esp32:esp32:esp32da:FlashSize= ",
    };

    board_request request;
    std::string error;
    for (const char* text : malformed) {
        check(!parse_board_request(text, request, error) && !error.empty(), text);
    }

    parse_board_request("esp32:esp32", request, error);
    check(error == "esp32:esp32 names no board. Name one as packager:architecture:board or by its ID",
          "a core alone names no board");

    parse_board_request("esp32::esp32da", request, error);
    check(error == "esp32::esp32da has an empty packager, architecture or board ID", "an empty name is named");

    parse_board_request("esp32:esp32:esp32da:FlashSize", request, error);
    check(error == "FlashSize is not a menu=choice option", "an option without a choice is named");
}

static void check_format() {
    check(format_fqbn("esp32", "esp32", "esp32da", {}) == "esp32:esp32:esp32da", "no options leave the FQBN bare");

    std::string fqbn = format_fqbn("esp32", "esp32", "esp32da", { { "FlashSize", "8M" }, { "PSRAM", "enabled" } });
    check(fqbn == "esp32:esp32:esp32da:FlashSize=8M,PSRAM=enabled", "options follow a colon, separated by commas");
}

int main() {
    check_board_ids();
    check_fqbns();
    check_options();
    check_failures();
    check_format();

    if (g_failures != 0) {
        printf("fqbn: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("fqbn: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
