/* zip and digest, run on the build machine: stored and deflated entries read however the bytes arrive, the checks
 * against each entry's CRC-32 and the central directory, each refusal, and SHA-256 */

#include "digest.hpp"
#include "installer.hpp"
#include "zip.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;
constexpr uint32_t LOCAL_HEADER_SIGNATURE = 0x04034b50;
constexpr uint32_t CENTRAL_HEADER_SIGNATURE = 0x02014b50;
constexpr uint32_t END_RECORD_SIGNATURE = 0x06054b50;
constexpr uint32_t ZIP64_END_RECORD_SIGNATURE = 0x06064b50;
constexpr uint16_t VERSION_NEEDED = 20;
constexpr uint16_t HOST_UNIX = 3;
constexpr uint16_t HOST_DOS = 0;
constexpr uint16_t METHOD_STORED = 0;
constexpr uint16_t METHOD_DEFLATED = 8;
constexpr uint16_t FLAG_ENCRYPTED = 1 << 0;
constexpr uint16_t FLAG_DATA_DESCRIPTOR = 1 << 3;
constexpr uint32_t ZIP64_MARKER = 0xffffffff;
constexpr uint16_t ZIP64_ENTRIES_MARKER = 0xffff;
constexpr uint16_t ZIP64_EXTRA_ID = 0x0001;
constexpr uint16_t TIMESTAMP_EXTRA_ID = 0x5455;
constexpr uint32_t EXECUTABLE_MODE = 0100755;
constexpr uint32_t LINK_MODE = 0120777;
constexpr uint32_t DOS_ATTRIBUTES = 0x00810020;

/* The reader inflates 64 KiB at a time. zlib reports a raw stream ending exactly there on the call that fills the
 * buffer or on the next, depending on its version. */
constexpr size_t INFLATE_OUTPUT_BYTES = 64 * 1024;
constexpr size_t LOCAL_HEADER_BYTES = 30;
constexpr size_t STORED_BLOCK_HEADER_BYTES = 5;
constexpr size_t STORED_BLOCK_BYTES = 65535;
constexpr size_t ZERO_MEMBERS = 200;
constexpr size_t ZERO_MEMBER_BYTES = 4096;

/* An entry the test archive holds, with the extra fields and comment real archives carry, and faults a test plants */
struct test_entry {
    std::string path;
    std::string data;
    bool        deflated = false;
    uint32_t    unix_mode = 0;
    bool        bad_crc = false;
    bool        central_size_differs = false;
    uint16_t    flags = 0;
    bool        unfinished = false;  // deflated without the stream's last block
    bool        unlisted = false;    // missing from the central directory
    int         level = Z_BEST_COMPRESSION;
    bool        trailing_byte = false;  // a byte after the deflated data, counted in the compressed size
    bool        understated = false;    // both headers state one byte less than the data holds
    bool        overstated = false;     // both headers state one byte more than the data holds
    int         method = -1;            // the method both headers state, -1 for the one the data uses
    uint32_t    compressed_delta = 0;   // added to the compressed size both headers state
    bool        zip64_sizes = false;    // the local header states zip64's size markers
    bool        zip64_extra = false;    // the local header carries zip64's extra field
    uint16_t    host = HOST_UNIX;
    uint32_t    attributes = 0;         // the central directory's external attributes, 0 for the Unix mode's
    std::string central_path = "";      // the path the central directory states, empty for the entry's
    bool        central_crc_differs = false;
    bool        central_compressed_differs = false;
    std::string comment = "entry comment";
};

/* Faults a test plants in the archive around its entries */
struct test_archive {
    std::string comment = "a commit id, as git archive writes";
    int         stated_entries = -1;  // the end record's count, -1 for the listed entries
    bool        zip64_record = false;  // a zip64 end record before the end record
    bool        entry_after_central = false;
    bool        byte_after_end = false;
};

struct read_result {
    std::vector<std::string> paths;
    std::vector<std::string> data;
    std::vector<zip_member>  members;
    std::string              error;
    bool                     read = false;
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

static void put16(std::string& out, uint32_t value) {
    out += static_cast<char>(value & 0xff);
    out += static_cast<char>((value >> 8) & 0xff);
}

static void put32(std::string& out, uint32_t value) {
    put16(out, value & 0xffff);
    put16(out, value >> 16);
}

static std::string raw_deflate(const std::string& data, bool finished, int level) {
    z_stream stream{};
    deflateInit2(&stream, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);

    std::string out(deflateBound(&stream, data.size()), '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());

    deflate(&stream, finished ? Z_FINISH : Z_SYNC_FLUSH);
    out.resize(stream.total_out);
    deflateEnd(&stream);

    return out;
}

static std::string extra_field(uint16_t id, const std::string& data) {
    std::string field;
    put16(field, id);
    put16(field, data.size());

    return field + data;
}

static std::string local_extra(const test_entry& entry) {
    if (entry.zip64_extra) {
        return extra_field(ZIP64_EXTRA_ID, std::string(16, '\0'));
    }

    return extra_field(TIMESTAMP_EXTRA_ID, std::string(5, '\1'));
}

static void put_local_header(std::string& out, const test_entry& entry, uint32_t crc, uint32_t compressed_size,
                             uint32_t stated_size, uint16_t method) {
    std::string extra = local_extra(entry);
    put32(out, LOCAL_HEADER_SIGNATURE);
    put16(out, VERSION_NEEDED);
    put16(out, entry.flags);
    put16(out, method);
    put32(out, 0);

    put32(out, crc);
    put32(out, entry.zip64_sizes ? ZIP64_MARKER : compressed_size);
    put32(out, entry.zip64_sizes ? ZIP64_MARKER : stated_size);
    put16(out, entry.path.size());
    put16(out, extra.size());
    out += entry.path + extra;
}

static void put_central_header(std::string& out, const test_entry& entry, uint32_t crc, uint32_t compressed_size,
                               uint32_t stated_size, uint16_t method, uint32_t offset) {
    std::string path = entry.central_path.empty() ? entry.path : entry.central_path;
    std::string extra = extra_field(TIMESTAMP_EXTRA_ID, std::string(1, '\1'));

    put32(out, CENTRAL_HEADER_SIGNATURE);
    put16(out, entry.host << 8 | VERSION_NEEDED);
    put16(out, VERSION_NEEDED);
    put16(out, entry.flags);
    put16(out, method);
    put32(out, 0);

    put32(out, crc ^ (entry.central_crc_differs ? 1 : 0));
    put32(out, compressed_size + (entry.central_compressed_differs ? 1 : 0));
    put32(out, stated_size + (entry.central_size_differs ? 1 : 0));
    put16(out, path.size());
    put16(out, extra.size());
    put16(out, entry.comment.size());

    put32(out, 0);
    put32(out, entry.attributes ? entry.attributes : entry.unix_mode << 16);
    put32(out, offset);
    out += path + extra + entry.comment;
}

static void put_end_record(std::string& out, uint16_t entries, uint32_t central_size, uint32_t central_offset,
                           const std::string& comment) {
    put32(out, END_RECORD_SIGNATURE);
    put32(out, 0);
    put16(out, entries);
    put16(out, entries);

    put32(out, central_size);
    put32(out, central_offset);
    put16(out, comment.size());
    out += comment;
}

static uint32_t crc_of(const std::string& data) {
    return crc32(0, reinterpret_cast<const Bytef*>(data.data()), data.size());
}

static std::string build_archive(const std::vector<test_entry>& entries, const test_archive& options = {}) {
    std::string archive;
    std::string central;
    uint16_t listed = 0;
    for (const test_entry& entry : entries) {
        std::string stored = entry.deflated ? raw_deflate(entry.data, !entry.unfinished, entry.level) : entry.data;
        stored += entry.trailing_byte ? "x" : "";
        uint32_t stated_size = entry.data.size() - (entry.understated ? 1 : 0) + (entry.overstated ? 1 : 0);
        uint32_t crc = crc_of(entry.data) ^ (entry.bad_crc ? 1 : 0);

        uint32_t offset = archive.size();
        uint32_t compressed_size = stored.size() + entry.compressed_delta;
        uint16_t method = entry.method >= 0 ? entry.method : entry.deflated ? METHOD_DEFLATED : METHOD_STORED;
        put_local_header(archive, entry, crc, compressed_size, stated_size, method);
        archive += stored;
        if (!entry.unlisted) {
            put_central_header(central, entry, crc, compressed_size, stated_size, method, offset);
            listed++;
        }
    }

    uint32_t central_offset = archive.size();
    archive += central;
    if (options.entry_after_central) {
        test_entry late = { "a/late.txt", "late" };
        put_local_header(archive, late, crc_of(late.data), late.data.size(), late.data.size(), METHOD_STORED);
        archive += late.data;
    }

    if (options.zip64_record) {
        put32(archive, ZIP64_END_RECORD_SIGNATURE);
    }

    uint16_t stated_entries = options.stated_entries >= 0 ? options.stated_entries : listed;
    put_end_record(archive, stated_entries, central.size(), central_offset, options.comment);

    return archive + (options.byte_after_end ? "x" : "");
}

// Feeds the archive in pieces of `piece` bytes, the first one `first` bytes long when that is set
static read_result read_archive(const std::string& archive, size_t piece, size_t first = 0,
                                uint64_t unpacked_max = UINT64_MAX) {
    read_result result;
    zip_handlers handlers;
    handlers.begin_entry = [&](const zip_entry& entry, std::string&) {
        result.paths.push_back(entry.path);
        result.data.emplace_back();
        return true;
    };

    handlers.entry_bytes = [&](std::string_view bytes, std::string&) {
        result.data.back().append(bytes);
        return true;
    };

    handlers.end_entry = [](std::string&) {
        return true;
    };

    zip_reader reader(handlers, unpacked_max);
    size_t start = 0;
    while (start < archive.size()) {
        size_t length = start == 0 && first > 0 ? first : piece;
        if (!reader.feed(std::string_view(archive).substr(start, length), result.error)) {
            return result;
        }

        start += length;
    }

    result.read = reader.finish(result.error);
    result.members = reader.members();

    return result;
}

static void check_entries() {
    std::string text(100000, '\0');
    for (size_t i = 0; i < text.size(); i++) {
        text[i] = static_cast<char>("abcdefgh"[(i * 7 + i / 13) % 8]);
    }

    std::vector<test_entry> entries = {
        { "esp32-3.3.12/", "", false, 040755 },
        { "esp32-3.3.12/platform.txt", "name=ESP32 Arduino\n", false, 0100644 },
        { "esp32-3.3.12/tools/gen.py", text, true, EXECUTABLE_MODE },
        { "esp32-3.3.12/empty.txt", "", true, 0 },
    };

    std::string archive = build_archive(entries);
    for (size_t piece : { size_t(1), size_t(13), archive.size() }) {
        read_result result = read_archive(archive, piece);
        check(result.read, "the archive reads in any pieces, its extra fields and comments skipped");

        check(result.paths.size() == 4 && result.paths[1] == "esp32-3.3.12/platform.txt", "entries come in order");
        check(result.data.size() == 4 && result.data[2] == text, "a deflated entry inflates whole");
        check(result.data.size() == 4 && result.data[3].empty(), "an empty deflated entry reads");
        check(result.members.size() == 4 && result.members[2].unix_mode == EXECUTABLE_MODE, "modes come from Unix");
        check(result.members.size() == 4 && result.members[3].unix_mode == 0, "a mode of 0 stays 0");
    }

    test_entry from_dos = { "a/readme.txt", "hi" };
    from_dos.host = HOST_DOS;
    from_dos.attributes = DOS_ATTRIBUTES;
    read_result dos = read_archive(build_archive({ from_dos }), 64);
    check(dos.read && dos.members.size() == 1 && dos.members[0].unix_mode == 0,
          "an entry made off Unix has no mode, whatever its attributes hold");
}

static void check_output_boundaries() {
    for (size_t size : { INFLATE_OUTPUT_BYTES - 1, INFLATE_OUTPUT_BYTES, INFLATE_OUTPUT_BYTES + 1,
                         2 * INFLATE_OUTPUT_BYTES }) {
        std::string patterned(size, '\0');
        for (size_t i = 0; i < size; i++) {
            patterned[i] = static_cast<char>((i * 31 + i / 7) & 0xff);
        }

        std::string zeros(size, '\0');
        std::string archive = build_archive({ { "a/patterned", patterned, true }, { "a/zeros", zeros, true } });
        for (size_t piece : { size_t(1), size_t(7), size_t(4096), archive.size() }) {
            read_result result = read_archive(archive, piece);
            bool whole = result.read && result.data.size() == 2 && result.data[0] == patterned &&
                         result.data[1] == zeros;
            check(whole, "an entry ending at or one byte either side of the end of the inflate buffer reads whole");
        }
    }
}

// Level 0 writes stored blocks of 65535 bytes after 5-byte headers, so the first piece ending in the second block's
// first byte fills the inflate buffer as the piece runs out, and inflate has nothing to do until more bytes arrive
static void check_input_ending_as_buffer_fills() {
    test_entry entry = { "a/stored", std::string(2 * INFLATE_OUTPUT_BYTES, 'x'), true };
    entry.level = 0;
    size_t first = LOCAL_HEADER_BYTES + entry.path.size() + local_extra(entry).size() + STORED_BLOCK_HEADER_BYTES +
                   STORED_BLOCK_BYTES + STORED_BLOCK_HEADER_BYTES + 1;
    read_result result = read_archive(build_archive({ entry }), 1 << 20, first);
    check(result.read && result.data.size() == 1 && result.data[0] == entry.data,
          "a piece ending as the inflate buffer fills waits for the next");
}

static bool refuses(const std::vector<test_entry>& entries, const std::string& expected_error,
                    const test_archive& options = {}) {
    read_result result = read_archive(build_archive(entries, options), 5);
    if (!result.read && result.error == expected_error) {
        return true;
    }

    printf("  got: %s\n", result.read ? "an archive that reads" : result.error.c_str());

    return false;
}

static void check_refusals() {
    test_entry bad_crc = { "a/x.txt", "hello", true, 0, true };
    check(refuses({ bad_crc }, "a/x.txt does not match the size and CRC-32 its header states"), "a bad CRC fails");

    test_entry disagreeing = { "a/x.txt", "hello", false, 0, false, true };
    check(refuses({ disagreeing }, "the central directory disagrees with the entries about a/x.txt"),
          "a central directory stating another size fails");

    test_entry unfinished = { "a/x.txt", "hello", true };
    unfinished.unfinished = true;
    check(refuses({ unfinished }, "a/x.txt holds deflated data that is cut short"),
          "deflated data without its end fails");

    test_entry trailing = { "a/x.txt", "hello", true };
    trailing.trailing_byte = true;
    check(refuses({ trailing }, "a/x.txt holds bytes after its deflated data"), "bytes after the deflated data fail");

    test_entry understated = { "a/x.txt", "hello", true };
    understated.understated = true;
    check(refuses({ understated }, "a/x.txt holds more than the 4 bytes it states"),
          "an entry inflating past its stated size fails as it passes it");

    test_entry first = { "a/x.txt", "one" };
    test_entry again = { "a/x.txt", "two" };
    check(refuses({ first, again }, "the archive names a/x.txt twice"), "a path named twice fails");

    test_entry hidden = { "a/y.txt", "two" };
    hidden.unlisted = true;
    check(refuses({ first, hidden }, "the central directory and the archive hold different numbers of entries"),
          "an entry the central directory leaves out fails");

    std::string archive = build_archive({ first });
    read_result cut = read_archive(archive.substr(0, archive.size() - 10), 3);
    check(!cut.read && cut.error == "the archive ends before its central directory does", "a cut archive fails");
    check(!read_archive("PK\x05\x05" + archive, 64).read, "bytes that are no zip record fail");

    read_result link = read_archive(build_archive({ { "a/link", "target", false, LINK_MODE } }), 64);
    check(link.read && link.members[0].unix_mode == LINK_MODE, "a link's mode reaches the caller");
}

/* Each refusal starts from an entry that reads and breaks one property of it */
static void check_entry_refusals() {
    test_entry encrypted = { "a/x.txt", "hello" };
    encrypted.flags = FLAG_ENCRYPTED;
    check(refuses({ encrypted }, "a/x.txt is encrypted, which mcufw does not read"), "an encrypted entry fails");

    test_entry described = { "a/x.txt", "hello" };
    described.flags = FLAG_DATA_DESCRIPTOR;
    check(refuses({ described }, "a/x.txt states its sizes after its data, which mcufw does not read"),
          "a data descriptor fails");

    test_entry zip64_sizes = { "a/x.txt", "hello" };
    zip64_sizes.zip64_sizes = true;
    check(refuses({ zip64_sizes }, "a/x.txt is in zip64, which mcufw does not read"), "zip64's size markers fail");

    test_entry zip64_extra = { "a/x.txt", "hello" };
    zip64_extra.zip64_extra = true;
    check(refuses({ zip64_extra }, "a/x.txt is in zip64, which mcufw does not read"), "zip64's extra field fails");

    test_entry other_method = { "a/x.txt", "hello" };
    other_method.method = 12;
    check(refuses({ other_method }, "a/x.txt is compressed with method 12, not stored or deflated"),
          "a method other than stored or deflated fails");

    test_entry two_sizes = { "a/x.txt", "hello" };
    two_sizes.compressed_delta = 1;
    check(refuses({ two_sizes }, "a/x.txt is stored uncompressed but states two different sizes"),
          "a stored entry with two sizes fails");

    test_entry overstated = { "a/x.txt", "hello", true };
    overstated.overstated = true;
    check(refuses({ overstated }, "a/x.txt does not match the size and CRC-32 its header states"),
          "an entry inflating short of its stated size fails though its CRC-32 matches");

    test_entry full_folder = { "a/folder/", "data" };
    check(refuses({ full_folder }, "a/folder/ is a folder that holds data"), "a folder entry holding data fails");
}

static void check_central_refusals() {
    test_entry other_path = { "a/x.txt", "hello" };
    other_path.central_path = "a/y.txt";
    check(refuses({ other_path }, "the central directory disagrees with the entries about a/y.txt"),
          "a central entry naming another path fails");

    test_entry other_crc = { "a/x.txt", "hello" };
    other_crc.central_crc_differs = true;
    check(refuses({ other_crc }, "the central directory disagrees with the entries about a/x.txt"),
          "a central entry stating another CRC-32 fails");

    test_entry other_compressed = { "a/x.txt", "hello", true };
    other_compressed.central_compressed_differs = true;
    check(refuses({ other_compressed }, "the central directory disagrees with the entries about a/x.txt"),
          "a central entry stating another compressed size fails");

    test_entry plain = { "a/x.txt", "hello" };
    test_archive late_entry;
    late_entry.entry_after_central = true;
    check(refuses({ plain }, "the archive is no zip, or is corrupt", late_entry),
          "a local header after the central directory fails");

    test_archive zip64_count;
    zip64_count.stated_entries = ZIP64_ENTRIES_MARKER;
    check(refuses({ plain }, "the archive is zip64, which mcufw does not read", zip64_count),
          "zip64's entry count fails");

    test_archive zip64_record;
    zip64_record.zip64_record = true;
    check(refuses({ plain }, "the archive is zip64, which mcufw does not read", zip64_record),
          "a zip64 end record fails");

    test_archive trailing;
    trailing.byte_after_end = true;
    check(refuses({ plain }, "the archive holds bytes after its end", trailing), "a byte after the end record fails");

    test_entry unlisted = { "a/y.txt", "two" };
    unlisted.unlisted = true;
    test_archive counted;
    counted.stated_entries = 2;
    check(refuses({ plain, unlisted }, "the central directory and the archive hold different numbers of entries",
                  counted),
          "an end record counting the entries but not the central directory fails");
}

/* Members of zeros pack to a few bytes each, so a small archive can state many times its size */
static void check_unpacked_cap() {
    std::vector<test_entry> members;
    for (size_t i = 0; i < ZERO_MEMBERS; i++) {
        members.push_back({ "a/zeros-" + std::to_string(i), std::string(ZERO_MEMBER_BYTES, '\0'), true });
    }

    std::string archive = build_archive(members);
    uint64_t total = ZERO_MEMBERS * ZERO_MEMBER_BYTES;
    check(read_archive(archive, 4096, 0, total).read, "an archive unpacking to exactly its cap reads");

    read_result over = read_archive(archive, 4096, 0, total - 1);
    check(!over.read && over.error == "a/zeros-199 takes the archive past the 819199 bytes it may unpack to",
          "the member whose stated size passes the cap fails");
    check(over.paths.size() == ZERO_MEMBERS - 1, "the member passing the cap fails before it writes a byte");

    archive_download download = { "https://x/zeros.zip", "zeros.zip", "", archive.size() };
    read_result bomb = read_archive(archive, 4096, 0, unpacked_bytes_max(download));
    check(!bomb.read && unpacked_bytes_max(download) < total,
          "a small archive of many members fails the cap of 16 times its stated size");
}

static void check_digest() {
    sha256_digest empty;
    check(empty.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");

    sha256_digest pieces;
    pieces.update("a");
    pieces.update("bc");
    check(pieces.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 in pieces");
}

int main() {
    check_entries();
    check_output_boundaries();
    check_input_ending_as_buffer_fills();

    check_refusals();
    check_entry_refusals();
    check_central_refusals();
    check_unpacked_cap();
    check_digest();

    if (g_failures != 0) {
        printf("zip: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("zip: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
