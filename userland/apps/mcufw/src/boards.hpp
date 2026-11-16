#ifndef MCUFW_BOARDS_HPP
#define MCUFW_BOARDS_HPP

// A board chosen among the installed cores with its options, and the properties its recipes expand in, layered as
// arduino-cli 1.6.0 layers them

#include "cores.hpp"
#include "fqbn.hpp"
#include "properties.hpp"
#include "tools.hpp"

#include <string>
#include <vector>

struct selected_board {
    const installed_core*       core;           // owned by the caller's list of cores, which must outlive this
    const board_definition*     definition;
    std::vector<option_setting> given_options;  // as requested
    std::vector<option_setting> options;        // every menu's choice: defaults in menu order, then those given
};

// Finds the board `request` names, by an ID only when one installed core defines it, and fills `board.options`. Fails
// on an undeclared menu, a menu with no labeled choice, an option no menu holds, or a core or variant given a packager.
bool select_board(const std::vector<installed_core>& cores, const board_request& request, selected_board& board,
                  std::string& error);

// The FQBN with the options as given, which `build.fqbn` holds, as arduino-cli's `fqbn.String()` writes it
std::string requested_fqbn(const selected_board& board);

// The FQBN with every option in effect, in menu order, as a description's `board` event gives it
std::string full_fqbn(const selected_board& board);

// The core's platform properties, then the board's lines, its options, and the values arduino-cli's `ResolveFQBN`
// supplies with each located tool's paths. select_board refuses other cores' files, so every path is the board's own.
property_table board_properties(const selected_board& board, const std::vector<located_tool>& tools);

#endif // MCUFW_BOARDS_HPP
