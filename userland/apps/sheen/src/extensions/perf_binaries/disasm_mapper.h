#ifndef SHEEN_EXTENSIONS_PERF_BINARIES_DISASM_MAPPER_H
#define SHEEN_EXTENSIONS_PERF_BINARIES_DISASM_MAPPER_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sheen {

inline constexpr size_t UNMAPPED_DISASM_INDEX = static_cast<size_t>(-1);
inline constexpr size_t DISASM_MAX_INSTRUCTIONS = 262144;

/* Half-open contiguous run `[first_row, first_row + row_count)` in `disasm_map::instructions`. */
struct instruction_row_span {
    size_t first_row = 0;
    size_t row_count = 0;
};

struct disasm_instruction_row {
    uint64_t address = 0;
    std::vector<uint8_t> raw_bytes;
    std::string raw_bytes_text;
    std::string mnemonic_and_operands;
    std::string symbol_name;
    std::string source_file;
    int source_line = 0; // 1-based source line when DWARF line correlation exists, 0 otherwise
};

struct source_line_mapping {
    std::string source_file;
    int source_line = 0;
    std::vector<instruction_row_span> spans;
    size_t total_instructions = 0;
};

struct disasm_symbol_block {
    uint64_t start_address = 0;
    std::string symbol_name;
    instruction_row_span rows;
};

struct disasm_map {
    std::string primary_source_file;
    std::vector<disasm_symbol_block> symbols;
    std::vector<disasm_instruction_row> instructions;
    std::vector<source_line_mapping> line_mappings;
    std::vector<size_t> instruction_to_mapping;
};

struct address_sample {
    uint64_t address = 0;
    uint64_t weight = 0;
};

struct instruction_gutter_cost {
    size_t row_index = 0;
    uint64_t weight = 0;
    double percent = 0.0;
};

struct source_line_gutter_cost {
    std::string source_file;
    int source_line = 0;
    uint64_t weight = 0;
    double percent = 0.0;
};

struct disasm_gutter_profile {
    uint64_t total_matched_weight = 0;
    uint64_t denominator_weight = 0;
    std::vector<instruction_gutter_cost> instruction_costs;
    std::vector<source_line_gutter_cost> line_costs;
};

bool parse_objdump_disassembly(std::string_view objdump_output, disasm_map& mapped, std::string& error);
bool source_paths_match(std::string_view recorded_path, std::string_view query_path);
std::vector<instruction_row_span> spans_for_source_line(const disasm_map& mapped,
                                                        std::string_view source_file,
                                                        int source_line);
bool source_for_instruction_row(const disasm_map& mapped, size_t row_index, std::string& source_file,
                                int& source_line, instruction_row_span& enclosing_span);
std::vector<std::pair<int, int>> build_split_view_line_pairs(const disasm_map& mapped,
                                                             std::string_view source_file = {});
disasm_gutter_profile compute_disasm_gutter_profile(const disasm_map& mapped,
                                                    std::span<const address_sample> samples,
                                                    std::string_view source_file = {},
                                                    uint64_t denominator_override = 0);
std::string format_gutter_percentage(double percent, uint64_t duration_ns = 0);

} // namespace sheen

#endif // SHEEN_EXTENSIONS_PERF_BINARIES_DISASM_MAPPER_H
