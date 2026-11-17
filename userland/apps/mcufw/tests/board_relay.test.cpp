/* board_relay, run on the build machine with ELF files the test writes: a panic relayed at once and decoded after its
 * hash line, a mismatch stopping the decoding, and a rebuilt ELF starting it again */

#include "board_relay.hpp"

#include "files.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS        = 8;
constexpr uint32_t ALLOCATED_CODE     = 0x6;
constexpr uint32_t CODE_SIZE          = 0x20000;
constexpr size_t   ELF_HEADER_SIZE    = 52;
constexpr size_t   SECTION_ENTRY_SIZE = 40;
constexpr int64_t  NS_PER_SECOND      = 1000000000;
constexpr int64_t  PAST_NS            = 1700000000 * NS_PER_SECOND;

static const std::string PANIC = "abort() was called at PC 0x400d1683 on core 1\r\n";
static const std::string LOOP_LOOKUP = "0x400d1683\nloop()\n/sketches/Lamp/Lamp.ino:8\n";
static const std::string LOOP_LINE = "0x400d1683: loop() at /sketches/Lamp/Lamp.ino:8\r\n";

struct capture {
    FILE* out;
    FILE* err;
};

// A decoder and a relay on an ELF the test wrote, set up as the monitor sets them up, printing into `out`
struct session {
    file_stamp    elf;
    crash_decoder decoder;
    capture       out;
    reporter      report;
    board_relay   relay;

    explicit session(const std::string& path);
};

static uint64_t    g_checks;
static uint64_t    g_failures;
static uint32_t    g_lookups;
static bool        g_lookup_fails;
static std::string g_root;

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

static std::string read_back(FILE* file) {
    std::string text;
    char chunk[4096];
    rewind(file);
    while (size_t count = fread(chunk, 1, sizeof(chunk), file)) {
        text.append(chunk, count);
    }

    fclose(file);
    return text;
}

static void put_u16(std::string& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<char>(value & 0xff);
    bytes[offset + 1] = static_cast<char>(value >> 8);
}

static void put_u32(std::string& bytes, size_t offset, uint32_t value) {
    put_u16(bytes, offset, static_cast<uint16_t>(value & 0xffff));
    put_u16(bytes, offset + 2, static_cast<uint16_t>(value >> 16));
}

// A 32-bit little-endian ELF whose one executable section starts at `code_start`, so each start is another build
static void write_elf(const std::string& path, uint32_t code_start, int64_t modified_ns) {
    std::string bytes(ELF_HEADER_SIZE + 2 * SECTION_ENTRY_SIZE, '\0');
    bytes.replace(0, 4, "\x7f" "ELF");
    bytes[4] = 1;
    bytes[5] = 1;
    bytes[6] = 1;

    put_u32(bytes, 32, ELF_HEADER_SIZE);
    put_u16(bytes, 40, ELF_HEADER_SIZE);
    put_u16(bytes, 46, SECTION_ENTRY_SIZE);
    put_u16(bytes, 48, 2);

    put_u32(bytes, ELF_HEADER_SIZE + SECTION_ENTRY_SIZE + 8, ALLOCATED_CODE);
    put_u32(bytes, ELF_HEADER_SIZE + SECTION_ENTRY_SIZE + 12, code_start);
    put_u32(bytes, ELF_HEADER_SIZE + SECTION_ENTRY_SIZE + 20, CODE_SIZE);

    FILE* file = fopen(path.c_str(), "wb");
    if (!file || fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        printf("  cannot write %s\n", path.c_str());
        exit(1);
    }

    fclose(file);
    timespec times[2] = { { modified_ns / NS_PER_SECOND, modified_ns % NS_PER_SECOND },
                          { modified_ns / NS_PER_SECOND, modified_ns % NS_PER_SECOND } };
    utimensat(AT_FDCWD, path.c_str(), times, 0);
}

// A file that is no ELF file yet, as a build in the middle of writing one leaves it
static void write_unreadable(const std::string& path, int64_t modified_ns) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file || fputs("not an elf", file) < 0) {
        printf("  cannot write %s\n", path.c_str());
        exit(1);
    }

    fclose(file);
    timespec times[2] = { { modified_ns / NS_PER_SECOND, modified_ns % NS_PER_SECOND },
                          { modified_ns / NS_PER_SECOND, modified_ns % NS_PER_SECOND } };
    utimensat(AT_FDCWD, path.c_str(), times, 0);
}

static std::string fake_look_up(const std::string&, const std::vector<uint32_t>& addresses) {
    g_lookups++;
    return !g_lookup_fails && addresses == std::vector<uint32_t>{ 0x400d1683 } ? LOOP_LOOKUP : "";
}

static std::string hash_line(const file_stamp& elf) {
    return "ELF file SHA256: " + elf.digest.substr(0, 9) + "\r\n";
}

static file_stamp stamped(const std::string& path) {
    file_stamp stamp;
    stamp_file(path, true, stamp);
    return stamp;
}

static std::vector<address_range> ranges_of(const std::string& path) {
    std::vector<address_range> ranges;
    std::string error;
    read_executable_ranges(path, ranges, error);
    return ranges;
}

session::session(const std::string& path)
    : elf(stamped(path)), decoder(ranges_of(path), elf.digest), out({ tmpfile(), tmpfile() }),
      report(false, out.out, out.err), relay(decoder, elf, fake_look_up, report) {}

static void check_release_on_hash() {
    std::string path = g_root + "/release.elf";
    write_elf(path, 0x400d0020, PAST_NS);
    session board(path);
    g_lookups = 0;

    board.relay.take_bytes(PANIC);
    board.relay.take_bytes(hash_line(board.elf));
    board.relay.take_bytes("abort() was called at PC 0x400d");
    board.relay.take_bytes("1683 on core 1\r\n");
    board.relay.take_bytes({});

    std::string expected = PANIC + hash_line(board.elf) + LOOP_LINE + PANIC;
    check(read_back(board.out.out) == expected,
          "decoded lines follow the hash line, and silence drops a later panic's once the board printed one");
    check(g_lookups == 1, "an address is looked up once, a line split across reads included");
    fclose(board.out.err);
}

static void check_board_without_hash() {
    std::string path = g_root + "/hashless.elf";
    write_elf(path, 0x400d0020, PAST_NS);
    session board(path);

    board.relay.take_bytes("abort() was called ");
    board.relay.take_bytes("at PC 0x400d1683 ");
    board.relay.take_bytes("on core 1\r\n");
    board.relay.take_bytes({});
    board.relay.take_bytes(PANIC + hash_line(board.elf));

    std::string expected = PANIC + LOOP_LINE + PANIC + hash_line(board.elf) + LOOP_LINE;
    check(read_back(board.out.out) == expected,
          "silence releases a panic split over three reads, and a hash line in the same read releases the next");
    fclose(board.out.err);
}

static void check_failed_lookup() {
    std::string path = g_root + "/unplaced.elf";
    write_elf(path, 0x400d0020, PAST_NS);
    session board(path);

    g_lookup_fails = true;
    board.relay.take_bytes(PANIC);
    board.relay.take_bytes({});
    g_lookup_fails = false;
    check(read_back(board.out.out) == PANIC, "relays a panic whose lookup failed with no decoded line");
    fclose(board.out.err);
}

static void check_addr2line_names() {
    check(addr2line_beside("/usr/avr/bin/avr-gcc") == "/usr/avr/bin/avr-addr2line",
          "finds avr-addr2line beside avr-gcc");
    check(addr2line_beside("/opt/xtensa/bin/xtensa-esp32-elf-gcc") == "/opt/xtensa/bin/xtensa-esp32-elf-addr2line",
          "finds xtensa-esp32-elf-addr2line beside xtensa-esp32-elf-gcc");
    check(addr2line_beside("/usr/bin/cc").empty(), "finds none beside a compiler named without gcc");
}

static void check_long_line() {
    std::string path = g_root + "/long.elf";
    write_elf(path, 0x400d0020, PAST_NS);
    session board(path);
    g_lookups = 0;

    std::string line = std::string(LINE_BYTES_MAX, 'x') + " 0x400d1683\r\n";
    board.relay.take_bytes(line);
    board.relay.take_bytes({});
    check(read_back(board.out.out) == line && g_lookups == 0,
          "a line past the limit is relayed whole and only its start is searched");
    fclose(board.out.err);
}

static void check_rebuild_after_mismatch() {
    std::string path = g_root + "/mismatch.elf";
    write_elf(path, 0x400d0020, PAST_NS);
    session board(path);
    g_lookups = 0;

    board.relay.take_bytes(PANIC);
    board.relay.take_bytes("ELF file SHA256: 0123456789\r\n");
    board.relay.take_bytes(PANIC);
    check(g_lookups == 1, "looks nothing up once the board runs another build");

    write_elf(path, 0x400d0000, PAST_NS + NS_PER_SECOND);
    file_stamp rebuilt = stamped(path);
    board.relay.take_bytes(PANIC);
    board.relay.take_bytes(hash_line(rebuilt));

    std::string expected = PANIC + "ELF file SHA256: 0123456789\r\n" +
                           "Decoding   off, the board runs another build than " + path + "\r\n" + PANIC + PANIC +
                           "Decoding   " + path + "\r\n" + hash_line(rebuilt) + LOOP_LINE;
    check(read_back(board.out.out) == expected, "a rebuilt ELF starts the decoding a mismatch stopped");
    check(g_lookups == 2, "and looks addresses up again in it");
    fclose(board.out.err);
}

static void check_unreadable_rebuild() {
    std::string path = g_root + "/unreadable.elf";
    write_elf(path, 0x400d0020, PAST_NS);
    session board(path);
    g_lookups = 0;

    write_unreadable(path, PAST_NS + NS_PER_SECOND);
    std::vector<address_range> ranges;
    std::string unreadable;
    read_executable_ranges(path, ranges, unreadable);
    board.relay.take_bytes(PANIC);
    board.relay.take_bytes(PANIC);

    write_elf(path, 0x400d0000, PAST_NS + 2 * NS_PER_SECOND);
    file_stamp rebuilt = stamped(path);
    board.relay.take_bytes(PANIC);
    board.relay.take_bytes(hash_line(rebuilt));

    std::string expected = PANIC + "Decoding   off, " + unreadable + "\r\n" + PANIC + PANIC + "Decoding   " + path +
                           "\r\n" + hash_line(rebuilt) + LOOP_LINE;
    check(read_back(board.out.out) == expected,
          "a rebuilt ELF that cannot be read stops decoding once, until a read succeeds");
    check(g_lookups == 1, "looks nothing up in an ELF it cannot read");
    fclose(board.out.err);
}

static void check_rebuild_while_decoding() {
    std::string path = g_root + "/rebuild.elf";
    write_elf(path, 0x400d0020, PAST_NS);
    session board(path);
    g_lookups = 0;

    board.relay.take_bytes(PANIC);
    board.relay.take_bytes(hash_line(board.elf));

    write_elf(path, 0x400d0000, PAST_NS + NS_PER_SECOND);
    file_stamp rebuilt = stamped(path);
    board.relay.take_bytes(PANIC);
    board.relay.take_bytes(hash_line(rebuilt));

    std::string expected = PANIC + hash_line(board.elf) + LOOP_LINE + PANIC + hash_line(rebuilt) + LOOP_LINE;
    check(read_back(board.out.out) == expected, "a crash at an address looked up before decodes with the rebuilt ELF");
    check(g_lookups == 2, "since the rebuild forgets the old lookups");
    fclose(board.out.err);
}

int main() {
    char root[] = "/tmp/mcufw-relay-XXXXXX";
    if (!mkdtemp(root)) {
        printf("  cannot make a temporary folder\n");
        return 1;
    }

    g_root = root;

    check_release_on_hash();
    check_board_without_hash();
    check_failed_lookup();
    check_long_line();

    check_rebuild_after_mismatch();
    check_unreadable_rebuild();
    check_rebuild_while_decoding();
    check_addr2line_names();

    std::string error;
    if (!remove_tree(g_root, error)) {
        printf("  %s\n", error.c_str());
    }

    if (g_failures != 0) {
        printf("board_relay: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("board_relay: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
