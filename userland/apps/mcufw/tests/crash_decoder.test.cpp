/* find_code_addresses, format_addr2line_output and crash_decoder, run on the build machine with an ESP32 build's
 * executable sections: which addresses decode, addr2line's lines, and a panic's lines held until its hash line */

#include "crash_decoder.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

static const char* const ELF_SHA256 = "f29faa75d1c2b3a4958677a6b5c4d3e2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6";
static const char* const OTHER_SHA256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static const char* const ABORT_LINE = "abort() was called at PC 0x400d1683 on core 1";
static const char* const BACKTRACE_LINE =
    "Backtrace: 0x40087578:0x3ffb2050 0x4008d2e5:0x3ffb2090 0x400d1683:0x3ffb2110 0x400d3d98:0x3ffb2130";
static const char* const HASH_LINE = "ELF file SHA256: f29faa75d";

static const char* const LOOP_LOOKUP =
    "0x400d1683\n"
    "loop()\n"
    "/sketches/CrashAbort/CrashAbort.ino:8\n"
    "loop()\n"
    "/sketches/CrashAbort/CrashAbort.ino:19\n";

static const char* const BACKTRACE_LOOKUP =
    "0x40087578\r\n"
    "panic_abort\r\n"
    "/esp-idf/components/esp_system/panic.c:477\r\n"
    "0x4008d2e5\r\n"
    "abort\r\n"
    "/esp-idf/components/newlib/src/abort.c:38\r\n"
    "0x400d3d98\r\n"
    "loopTask(void*)\r\n"
    "/var/lib/mcufw/hardware/esp32/esp32/3.3.12/cores/esp32/main.cpp:82 (discriminator 1)\r\n";

static const std::vector<address_range> ESP32_RANGES = {
    { 0x40080000, 0x40080400 },
    { 0x40080400, 0x40090000 },
    { 0x400d0020, 0x40120000 },
};

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

static void check_addresses() {
    std::vector<uint32_t> registers = find_code_addresses(
        "PC      : 0x400d167d  PS      : 0x00060730  A0      : 0x800d3d9b  A1      : 0x3ffb2110", ESP32_RANGES);
    check(registers == std::vector<uint32_t>{ 0x400d167d }, "decodes the program counter and no data register");

    std::vector<uint32_t> backtrace = find_code_addresses(BACKTRACE_LINE, ESP32_RANGES);
    check(backtrace == std::vector<uint32_t>({ 0x40087578, 0x4008d2e5, 0x400d1683, 0x400d3d98 }),
          "decodes each backtrace frame's program counter and none of its stack pointers");

    check(find_code_addresses("EXCVADDR: 0x00000000  LBEG    : 0x400852a1", ESP32_RANGES) ==
              std::vector<uint32_t>{ 0x400852a1 },
          "leaves out an address outside every executable section");

    check(find_code_addresses("PC 0X400D1683", ESP32_RANGES) == std::vector<uint32_t>{ 0x400d1683 },
          "reads capital hex digits");
    check(find_code_addresses("0x400d16831234", ESP32_RANGES) == std::vector<uint32_t>{ 0x400d1683 },
          "reads the first eight digits of a longer number");
    check(find_code_addresses("0x400d1683,0x400d3d98", ESP32_RANGES) ==
              std::vector<uint32_t>({ 0x400d1683, 0x400d3d98 }),
          "reads addresses with no space between them");
    check(find_code_addresses("0x400d168 x400d1683", ESP32_RANGES).empty(),
          "needs 0x and eight hex digits");
    check(find_code_addresses("0x400d1683", {}).empty(), "decodes nothing without executable sections");
    check(find_code_addresses("0x40080400 0x400803ff", { { 0x40080000, 0x40080400 } }) ==
              std::vector<uint32_t>{ 0x400803ff },
          "leaves out the address just past a section");

    check(find_code_addresses("PC 0x40080400", ESP32_RANGES) == std::vector<uint32_t>{ 0x40080400 },
          "decodes the first byte of a section");
}

static void check_addr2line_output() {
    std::vector<std::string> lines = format_addr2line_output(LOOP_LOOKUP);
    check(lines == std::vector<std::string>({
                       "0x400d1683: loop() at /sketches/CrashAbort/CrashAbort.ino:8",
                       " (inlined by) loop() at /sketches/CrashAbort/CrashAbort.ino:19",
                   }),
          "names a function and the one it was inlined into");

    std::vector<std::string> backtrace = format_addr2line_output(BACKTRACE_LOOKUP);
    check(backtrace.size() == 3 && backtrace[2] == "0x400d3d98: loopTask(void*) at "
                                                   "/var/lib/mcufw/hardware/esp32/esp32/3.3.12/cores/esp32/"
                                                   "main.cpp:82 (discriminator 1)",
          "reads lines ending in CR LF and keeps a discriminator");

    check(format_addr2line_output("0x40080400\n??\n??:0\n").empty(), "leaves out an address it cannot place");
    check(format_addr2line_output("0x40080400\n??\n??:?\n0x400d1683\nloop()\n/s/a.ino:8\n") ==
              std::vector<std::string>{ "0x400d1683: loop() at /s/a.ino:8" },
          "keeps the addresses it can place among those it cannot");
    check(format_addr2line_output("").empty(), "reads empty output");
    check(format_addr2line_output("0x400d1683\nfoo\n??:0\n") == std::vector<std::string>{ "0x400d1683: foo at ??:0" },
          "keeps a frame whose function is known though its place is not");
}

static void look_up(crash_decoder& decoder, const char* line, const char* output) {
    std::vector<uint32_t> addresses = decoder.take_line(line);
    if (!addresses.empty()) {
        decoder.take_lookup(addresses, output);
    }
}

static void check_held_until_hash() {
    crash_decoder decoder(ESP32_RANGES, ELF_SHA256);
    check(decoder.take_line(ABORT_LINE) == std::vector<uint32_t>{ 0x400d1683 }, "names the address to look up");
    decoder.take_lookup({ 0x400d1683 }, LOOP_LOOKUP);
    check(decoder.take_ready_lines().empty(), "holds the panic's lines");

    check(decoder.take_line(BACKTRACE_LINE) == std::vector<uint32_t>({ 0x40087578, 0x4008d2e5, 0x400d3d98 }),
          "looks each address up once a session");
    decoder.take_lookup({ 0x40087578, 0x4008d2e5, 0x400d3d98 }, BACKTRACE_LOOKUP);
    check(decoder.take_line("").empty() && decoder.take_ready_lines().empty(), "holds them past a blank line");

    check(decoder.take_line(HASH_LINE).empty(), "takes the hash line");
    std::vector<std::string> block = decoder.take_ready_lines();
    check(block.size() == 7, "releases every held line at the hash line");
    check(block.size() == 7 && block[0] == "0x400d1683: loop() at /sketches/CrashAbort/CrashAbort.ino:8" &&
              block[2] == "0x40087578: panic_abort at /esp-idf/components/esp_system/panic.c:477" &&
              block[4] == "0x400d1683: loop() at /sketches/CrashAbort/CrashAbort.ino:8",
          "keeps the lines in the board's order, repeating a frame's lines");
    check(decoder.take_ready_lines().empty(), "hands each line over once");

    check(decoder.take_line(ABORT_LINE).empty(), "decodes a known address with no lookup");
    check(decoder.take_ready_lines().empty(), "holds the next panic's lines");
    decoder.take_line("ELF file SHA256:  F29FAA75D1C2\r");
    check(decoder.take_ready_lines().size() == 2, "compares the digits ignoring case");
    check(decoder.decoding(), "keeps decoding while the hashes agree");

    decoder.take_line(ABORT_LINE);
    decoder.release_held();
    check(decoder.take_ready_lines().empty(), "drops a report without a hash line once the board printed one");
}

static void check_other_build() {
    crash_decoder decoder(ESP32_RANGES, OTHER_SHA256);
    look_up(decoder, ABORT_LINE, LOOP_LOOKUP);
    decoder.take_line(HASH_LINE);
    check(decoder.take_ready_lines().empty(), "drops the lines of a panic from another build");
    check(!decoder.decoding(), "stops decoding once the board runs another build");

    check(decoder.take_line(ABORT_LINE).empty(), "looks nothing up after another build's hash line");
    decoder.release_held();
    check(decoder.take_ready_lines().empty(), "holds nothing to release after another build's hash line");

    decoder.reload(ESP32_RANGES, ELF_SHA256);
    check(decoder.decoding(), "decodes again once the ELF is rebuilt");
    check(decoder.take_line(ABORT_LINE) == std::vector<uint32_t>{ 0x400d1683 },
          "looks addresses up again after a rebuild");
}

static void check_silence() {
    crash_decoder decoder(ESP32_RANGES, ELF_SHA256);
    look_up(decoder, ABORT_LINE, LOOP_LOOKUP);
    decoder.release_held();
    check(decoder.take_ready_lines().size() == 2, "releases the held lines when the board falls silent");

    check(SILENCE_BEFORE_RELEASE_NS == 500000000, "waits half a second of silence");

    crash_decoder digitless(ESP32_RANGES, ELF_SHA256);
    look_up(digitless, BACKTRACE_LINE, BACKTRACE_LOOKUP);
    digitless.take_line("ELF file SHA256: ");
    digitless.take_line("ELF file SHA256: zz");
    check(digitless.take_ready_lines().empty() && digitless.decoding(),
          "keeps holding through hash lines without digits");

    digitless.release_held();
    check(digitless.take_ready_lines().size() == 3, "reads a hash line without digits as an ordinary line");

    crash_decoder partial(ESP32_RANGES, ELF_SHA256);
    look_up(partial, "PC 0x400d1683 0x400d3d98", "0x400d1683\nloop()\n/s/a.ino:8\n");
    check(partial.take_line("PC 0x400d3d98").empty(), "does not look up again an address addr2line left out");

    crash_decoder unplaced(ESP32_RANGES, ELF_SHA256);
    look_up(unplaced, "PC 0x40080400", "0x40080400\n??\n??:0\n");
    unplaced.release_held();
    check(unplaced.take_ready_lines().empty(), "holds no line for an address addr2line cannot place");
    check(unplaced.take_line("PC 0x40080400").empty(), "does not look an unplaced address up again");
}

int main() {
    check_addresses();
    check_addr2line_output();
    check_held_until_hash();
    check_other_build();
    check_silence();

    if (g_failures != 0) {
        printf("crash_decoder: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("crash_decoder: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
