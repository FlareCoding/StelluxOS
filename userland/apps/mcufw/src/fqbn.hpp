#ifndef MCUFW_FQBN_HPP
#define MCUFW_FQBN_HPP

// Boards as a person names them: Arduino's fully qualified board name (FQBN, `packager:architecture:board`) or the
// board's ID alone, either one optionally followed by options, as in `esp32da:FlashSize=8M,PSRAM=enabled`

#include <string>
#include <string_view>
#include <vector>

struct option_setting {
    std::string menu_id;
    std::string choice_id;
};

struct board_request {
    std::string                 packager;      // empty when only the board's ID was given
    std::string                 architecture;  // empty when only the board's ID was given
    std::string                 board_id;
    std::vector<option_setting> options;       // in the order given, naming each menu at most once
};

// Fails on text that is neither form, an empty name, or an option that is not `menu=choice`. Option names and choices
// are trimmed, and a menu named twice keeps its last choice, as arduino-cli does.
bool parse_board_request(std::string_view text, board_request& request, std::string& error);

// `packager:architecture:board`, then `:menu=choice,...` when `options` holds any
std::string format_fqbn(const std::string& packager, const std::string& architecture, const std::string& board_id,
                        const std::vector<option_setting>& options);

#endif // MCUFW_FQBN_HPP
