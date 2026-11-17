#include "upload.hpp"

#include "recipes.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

constexpr std::string_view SERIAL_UPLOAD_PREFIX  = "upload.serial";
constexpr std::string_view DEVICE_DIRECTORY      = "/dev/";
constexpr const char*      SERIAL_PROTOCOL       = "serial";
constexpr const char*      SERIAL_PROTOCOL_LABEL = "Serial Port";

static bool is_blank(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
}

// `properties` with each `upload.serial.*` key over its `upload.*` key and the upload tool's keys over everything, as
// arduino-cli builds the properties a serial upload expands in
static bool read_upload_properties(const property_table& properties, property_table& upload, std::string& tool,
                                   std::string& error) {
    const std::string* named = properties.find("upload.tool.serial");
    if (!named) {
        named = properties.find("upload.tool.default");
    }

    if (!named) {
        named = properties.find("upload.tool");
    }

    if (!named || named->empty()) {
        error = "the board names no upload tool";
        return false;
    }

    if (named->find(':') != std::string::npos) {
        error = "the board's upload tool " + *named + " belongs to another platform";
        return false;
    }

    tool = *named;
    upload = properties;
    property_table serial_keys = properties.subtree(SERIAL_UPLOAD_PREFIX);
    for (const std::string& key : serial_keys.keys()) {
        upload.set("upload." + key, serial_keys.get(key));
    }

    upload.merge(upload.subtree("tools." + tool));

    return true;
}

bool upload_words(const property_table& build_properties, const std::string& port, bool verbose,
                  std::vector<std::string>& words, std::string& error) {
    property_table upload;
    std::string tool;
    if (!read_upload_properties(build_properties, upload, tool, error)) {
        return false;
    }

    if (!upload.find("upload.protocol")) {
        error = tool + " needs a programmer to upload, and mcufw uploads through serial ports only";
        return false;
    }

    const std::string* verbosity = upload.find(verbose ? "upload.params.verbose" : "upload.params.quiet");
    if (verbosity) {
        upload.set("upload.verbose", *verbosity);
    }

    upload.set("upload.verify", upload.get("upload.params.noverify"));

    upload.set("serial.port", port);
    upload.set("serial.port.file", port.starts_with(DEVICE_DIRECTORY) ? port.substr(DEVICE_DIRECTORY.size()) : port);
    upload.set("upload.port.address", port);
    upload.set("upload.port.label", port);
    upload.set("upload.port.protocol", SERIAL_PROTOCOL);
    upload.set("upload.port.protocolLabel", SERIAL_PROTOCOL_LABEL);

    const std::string* pattern = upload.find("upload.pattern");
    if (!pattern) {
        error = "the upload tool " + tool + " has no upload pattern";
        return false;
    }

    words.clear();
    if (is_blank(*pattern)) {
        return true;
    }

    return recipe_words(upload, "upload.pattern", words, error);
}

bool wants_1200_baud_touch(const property_table& board_properties) {
    property_table upload;
    std::string tool;
    std::string error;
    return read_upload_properties(board_properties, upload, tool, error) &&
           property_is_true(upload, "upload.use_1200bps_touch");
}

bool waits_for_upload_port(const property_table& board_properties) {
    property_table upload;
    std::string tool;
    std::string error;
    return read_upload_properties(board_properties, upload, tool, error) &&
           property_is_true(upload, "upload.wait_for_upload_port");
}

int last_percentage(std::string_view line) {
    size_t percent = line.rfind('%');
    while (percent != std::string_view::npos) {
        size_t start = percent;
        while (start > 0 && (std::isdigit(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '.')) {
            start--;
        }

        std::string_view whole = line.substr(start, percent - start);
        whole = whole.substr(0, whole.find('.'));
        int value = 0;
        std::from_chars_result parsed = std::from_chars(whole.data(), whole.data() + whole.size(), value);
        if (!whole.empty() && parsed.ec == std::errc() && parsed.ptr == whole.data() + whole.size()) {
            return value;
        }

        if (percent == 0) {
            break;
        }

        percent = line.rfind('%', percent - 1);
    }

    return -1;
}
