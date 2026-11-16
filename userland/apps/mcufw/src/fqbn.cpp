#include "fqbn.hpp"

constexpr std::string_view SPACES = " \t\n\v\f\r";

static std::string_view trimmed(std::string_view text) {
    size_t start = text.find_first_not_of(SPACES);
    if (start == std::string_view::npos) {
        return {};
    }

    size_t end = text.find_last_not_of(SPACES);

    return text.substr(start, end - start + 1);
}

static std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }

        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

static bool parse_options(std::string_view text, std::vector<option_setting>& options, std::string& error) {
    for (std::string_view pair : split(text, ',')) {
        size_t equals = pair.find('=');
        std::string menu_id(trimmed(pair.substr(0, equals)));
        std::string choice_id(equals == std::string_view::npos ? "" : trimmed(pair.substr(equals + 1)));
        if (menu_id.empty() || choice_id.empty()) {
            error = std::string(pair) + " is not a menu=choice option";
            return false;
        }

        std::erase_if(options, [&](const option_setting& earlier) {
            return earlier.menu_id == menu_id;
        });
        options.push_back({ menu_id, choice_id });
    }

    return true;
}

bool parse_board_request(std::string_view text, board_request& request, std::string& error) {
    request = {};
    std::vector<std::string_view> parts = split(text, ':');
    bool is_board_id = parts.size() == 1;
    bool is_board_id_with_options = parts.size() == 2 && parts[1].find('=') != std::string_view::npos;
    bool is_fqbn = parts.size() == 3 || parts.size() == 4;
    if (!is_board_id && !is_board_id_with_options && !is_fqbn) {
        error = std::string(text) + " names no board. Name one as packager:architecture:board or by its ID";
        return false;
    }

    size_t name_count = is_fqbn ? 3 : 1;
    for (size_t i = 0; i < name_count; i++) {
        if (parts[i].empty()) {
            error = std::string(text) + " has an empty packager, architecture or board ID";
            return false;
        }
    }

    if (is_fqbn) {
        request.packager = parts[0];
        request.architecture = parts[1];
    }

    request.board_id = parts[name_count - 1];
    bool has_options = parts.size() > name_count;

    return !has_options || parse_options(parts[name_count], request.options, error);
}

std::string format_fqbn(const std::string& packager, const std::string& architecture, const std::string& board_id,
                        const std::vector<option_setting>& options) {
    std::string fqbn = packager + ":" + architecture + ":" + board_id;
    for (size_t i = 0; i < options.size(); i++) {
        fqbn += i == 0 ? ":" : ",";
        fqbn += options[i].menu_id + "=" + options[i].choice_id;
    }

    return fqbn;
}
