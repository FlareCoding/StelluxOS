/* Host tests for `disasm_mapper`: GNU and `llvm-objdump` `-d -l -C` parsing, AArch64 `dc`,
 * wrapped continuations, `..` path normalization, multi-CU spans, and gutter percentages. */

#include "extensions/perf_binaries/disasm_mapper.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace sheen;

static constexpr uint32_t MAX_FAILURE_PRINTS = 12;

static uint64_t g_checks = 0;
static uint64_t g_failures = 0;

static void check(bool holds, const char* description) {
    g_checks++;
    if (!holds && ++g_failures <= MAX_FAILURE_PRINTS) {
        std::printf("  FAIL: %s\n", description);
    }
}

static void test_gnu_x86_64_intel_and_bidirectional_spans() {
    const char* sample_objdump =
        "\n"
        "build/stlxdm:     file format elf64-x86-64\n"
        "Disassembly of section .text:\n"
        "0000000000414280 <server::compose_damage(std::span<rect const>, int)>:\n"
        "compose_damage():\n"
        "  414280:\t55                   \tpush   rbp\n"
        "/src/stlxdm/build/../src/window.cpp:1668\n"
        "  414281:\t48 89 e5             \tmov    rbp,rsp\n"
        "  414284:\t48 8d 54 24 20       \tlea    rdx,[rsp+0x20]\n"
        "/src/stlxdm/src/window.cpp:1669 (discriminator 2)\n"
        "  414289:\t48 c7 85 70 ff ff ff \tmov    QWORD PTR [rbp-0x90],0x12345678\n"
        "  414290:\t78 56 34 12 \n"
        "/src/stlxdm/include/rect.h:1668\n"
        "  414294:\t89 f8                \tmov    eax,edi\n"
        "src/window.cpp:1668\n"
        "  414296:\t48 83 c0 01          \tadd    rax,0x1\n"
        "  41429a:\t75 e9                \tjne    414285 <server::compose_damage+0x5>\n"
        "??:0\n"
        "  41429c:\t5d                   \tpop    rbp\n"
        "  41429d:\tc3                   \tret\n"
        "\n"
        "0000000000414300 <helper_stub>:\n"
        "  414300:\t90                   \tnop\n";

    disasm_map mapped;
    std::string error;
    check(parse_objdump_disassembly(sample_objdump, mapped, error) && error.empty(),
          "parses GNU x86_64 Intel objdump -d -l -C output");
    if (mapped.instructions.size() != 10 || mapped.symbols.size() != 2) {
        check(false, "extracts 2 symbols and 10 folded instructions");
        return;
    }

    check(mapped.symbols[0].symbol_name == "server::compose_damage(std::span<rect const>, int)" &&
              mapped.symbols[0].rows.row_count == 9 && mapped.symbols[1].rows.first_row == 9,
          "preserves nested template angle brackets and symbol row spans");
    check(mapped.instructions[3].raw_bytes.size() == 11 && mapped.instructions[3].raw_bytes.back() == 0x12 &&
              mapped.instructions[3].source_line == 1669,
          "merges wrapped >7-byte continuation and strips discriminator suffix");
    check(mapped.instructions[7].source_line == 0 && mapped.instructions[9].source_line == 0,
          "clears attribution on ??:0 and symbol boundary");

    check(source_paths_match("/src/stlxdm/build/../src/window.cpp", "/src/stlxdm/src/window.cpp") &&
              !source_paths_match("ndow.cpp", "/src/stlxdm/src/window.cpp"),
          "normalizes '..' lexically and enforces '/' component boundaries");

    auto spans_1668 = spans_for_source_line(mapped, "/src/stlxdm/src/window.cpp", 1668);
    check(spans_1668.size() == 2 && spans_1668[0].first_row == 1 && spans_1668[0].row_count == 2 &&
              spans_1668[1].first_row == 5 && spans_1668[1].row_count == 2,
          "merges spans across build/../src/window.cpp and relative src/window.cpp CUs without rect.h");

    std::string rev_file;
    int rev_line = 0;
    instruction_row_span rev_span;
    check(!source_for_instruction_row(mapped, 0, rev_file, rev_line, rev_span) &&
              source_for_instruction_row(mapped, 5, rev_file, rev_line, rev_span) &&
              rev_line == 1668 && rev_span.first_row == 5 && rev_span.row_count == 2,
          "maps instruction row 5 back to enclosing span");

    auto split_pairs = build_split_view_line_pairs(mapped, "/src/stlxdm/src/window.cpp");
    check(split_pairs.size() == 2 && split_pairs[0] == std::make_pair(1668, 2) &&
              split_pairs[1] == std::make_pair(1669, 4),
          "builds 1-based sorted (source_line, first_disasm_line) pairs");

    std::vector<address_sample> samples = {
        { 0x414281, 760000 }, { 0x414286, 310000 }, { 0x414297, 1550000 },
        { 0x414294, 400000 }, { 0x414290, 950000 }, { 0x414301, 250000 },
    };

    disasm_gutter_profile gutter = compute_disasm_gutter_profile(mapped, samples, "/src/stlxdm/src/window.cpp", 8410000);
    check(gutter.total_matched_weight == 3970000 && gutter.line_costs.size() == 2 &&
              format_gutter_percentage(gutter.line_costs[0].percent, gutter.line_costs[0].weight) == "31.2%, 2.62 ms",
          "aggregates PC samples across multi-CU spans and excludes end-exclusive 0x414301");
}

static void test_llvm_objdump_and_aarch64_dc_mnemonics() {
    const char* llvm_output =
        "a0:\tfile format elf64-x86-64\n"
        "0000000000401000 <llvm_fn>:\n"
        "; ./src/math.cpp:10\n"
        "  401000: 48 c7 85 70 ff ff ff\tmov\tQWORD PTR [rbp - 0x90], 0x12345678\n"
        "  401007: 78 56 34 12\n"
        "; ./src/math.cpp:12\n"
        "  40100b: c3\tret\n";

    disasm_map llvm_map;
    std::string error;
    check(parse_objdump_disassembly(llvm_output, llvm_map, error) && llvm_map.instructions.size() == 2,
          "parses real llvm-objdump space-before-tab layout and skips hex-named file format banner");
    if (llvm_map.instructions.size() == 2) {
        check(llvm_map.instructions[0].raw_bytes.size() == 11 &&
                  llvm_map.instructions[0].raw_bytes.back() == 0x12 &&
                  llvm_map.instructions[1].raw_bytes.size() == 1 &&
                  llvm_map.instructions[1].mnemonic_and_operands == "ret",
              "preserves raw_bytes and wrapped continuation in llvm-objdump layout");
    }

    const char* aarch64_dc =
        "0000000000402000 <zero_cacheline>:\r\n"
        "src/cache.cpp:4\r\n"
        "  402000:\tA9BF7BFD \tstp\tx29, x30, [sp, #-16]!\r\n"
        "  402004:\td50b7420 \tdc\tzva, x0\r\n"
        "0000000000402008 <unattributed_next>:\r\n"
        "  402008:\td65f03c0 \tret\r\n";

    disasm_map arm_map;
    check(parse_objdump_disassembly(aarch64_dc, arm_map, error) && arm_map.instructions.size() == 3,
          "parses AArch64 word layout with uppercase hex and dc zva");
    if (arm_map.instructions.size() == 3) {
        check(arm_map.instructions[0].raw_bytes_text == "a9bf7bfd" &&
                  arm_map.instructions[0].raw_bytes[0] == 0xfdu &&
                  arm_map.instructions[1].mnemonic_and_operands == "dc zva, x0" &&
                  arm_map.instructions[2].source_line == 0,
              "lowercases 8-digit AArch64 word, preserves dc mnemonic, and resets line on next symbol");
    }

    const char* no_raw_dc =
        "0000000000402000 <no_raw_fn>:\n"
        "src/cache.cpp:9\n"
        "  402000:\tdc\tzva, x0\n"
        "  402004:\tfadd\ts0, s0, s1\n";

    disasm_map no_raw;
    check(parse_objdump_disassembly(no_raw_dc, no_raw, error) && no_raw.instructions.size() == 2 &&
              no_raw.instructions[0].raw_bytes.empty() &&
              no_raw.instructions[0].mnemonic_and_operands == "dc zva, x0" &&
              no_raw.instructions[1].mnemonic_and_operands == "fadd s0, s0, s1",
          "parses --no-show-raw-insn dc and fadd mnemonics without consuming 'dc' as 0xdc raw byte");
}

int main() {
    test_gnu_x86_64_intel_and_bidirectional_spans();
    test_llvm_objdump_and_aarch64_dc_mnemonics();

    if (g_failures != 0) {
        std::printf("disasm_mapper: %llu of %llu checks failed\n",
                    static_cast<unsigned long long>(g_failures),
                    static_cast<unsigned long long>(g_checks));
        return 1;
    }

    std::printf("disasm_mapper: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
