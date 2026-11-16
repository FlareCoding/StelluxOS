#include "zip.hpp"

#include "inflate.hpp"

#include <algorithm>
#include <unordered_set>

enum class zip_part { signature, local_header, entry_data, central_header, end_record, end_comment, done };

// An entry as its local header states it, which the central directory must repeat
struct local_entry {
    std::string path;
    uint32_t    crc;
    uint64_t    compressed_size;
    uint64_t    size;
};

struct zip_reader::zip_stream {
    zip_handlers                    handlers;
    zip_part                        part = zip_part::signature;
    std::string                     record;       // the bytes of the header or record being read
    size_t                          record_size;  // the bytes it needs, known in full once its fixed part is read
    bool                            in_central = false;
    std::vector<local_entry>        entries;
    std::unordered_set<std::string> paths;
    std::vector<zip_member>         members;
    local_entry                     entry;        // the entry whose data is read
    uint16_t                        method = 0;
    uint64_t                        remaining = 0;
    uint64_t                        written = 0;
    uint64_t                        unpacked_max = 0;   // the stated sizes the entries may add up to
    uint64_t                        unpacked_stated = 0;
    uint32_t                        crc = 0;
    z_stream                        inflater{};
    bool                            inflater_ready = false;
    bool                            inflate_ended = false;
    std::vector<unsigned char>      output;
};

using zip_stream = zip_reader::zip_stream;

constexpr uint32_t LOCAL_HEADER_SIGNATURE = 0x04034b50;
constexpr uint32_t CENTRAL_HEADER_SIGNATURE = 0x02014b50;
constexpr uint32_t END_RECORD_SIGNATURE = 0x06054b50;
constexpr uint32_t ZIP64_END_RECORD_SIGNATURE = 0x06064b50;
constexpr uint32_t ZIP64_LOCATOR_SIGNATURE = 0x07064b50;
constexpr size_t SIGNATURE_SIZE = 4;
constexpr size_t LOCAL_HEADER_SIZE = 30;
constexpr size_t CENTRAL_HEADER_SIZE = 46;
constexpr size_t END_RECORD_SIZE = 22;
constexpr size_t LOCAL_FLAGS_OFFSET = 6;
constexpr size_t LOCAL_METHOD_OFFSET = 8;
constexpr size_t LOCAL_CRC_OFFSET = 14;
constexpr size_t LOCAL_COMPRESSED_SIZE_OFFSET = 18;
constexpr size_t LOCAL_SIZE_OFFSET = 22;
constexpr size_t LOCAL_NAME_SIZE_OFFSET = 26;
constexpr size_t LOCAL_EXTRA_SIZE_OFFSET = 28;
constexpr size_t CENTRAL_MADE_BY_OFFSET = 4;
constexpr size_t CENTRAL_CRC_OFFSET = 16;
constexpr size_t CENTRAL_COMPRESSED_SIZE_OFFSET = 20;
constexpr size_t CENTRAL_SIZE_OFFSET = 24;
constexpr size_t CENTRAL_NAME_SIZE_OFFSET = 28;
constexpr size_t CENTRAL_EXTRA_SIZE_OFFSET = 30;
constexpr size_t CENTRAL_COMMENT_SIZE_OFFSET = 32;
constexpr size_t CENTRAL_EXTERNAL_ATTRIBUTES_OFFSET = 38;
constexpr size_t END_TOTAL_ENTRIES_OFFSET = 10;
constexpr size_t END_COMMENT_SIZE_OFFSET = 20;
constexpr size_t EXTRA_HEADER_SIZE = 4;
constexpr size_t EXTRA_SIZE_OFFSET = 2;
constexpr unsigned MADE_BY_HOST_SHIFT = 8;
constexpr unsigned UNIX_MODE_SHIFT = 16;
constexpr uint16_t METHOD_STORED = 0;
constexpr uint16_t METHOD_DEFLATED = 8;
constexpr uint16_t FLAG_ENCRYPTED = 1 << 0;
constexpr uint16_t FLAG_DATA_DESCRIPTOR = 1 << 3;
constexpr uint32_t ZIP64_MARKER = 0xffffffff;
constexpr uint16_t ZIP64_ENTRIES_MARKER = 0xffff;
constexpr uint16_t ZIP64_EXTRA_ID = 0x0001;
constexpr uint16_t HOST_UNIX = 3;
constexpr size_t INFLATE_OUTPUT_BYTES = 64 * 1024;

static uint16_t read16(std::string_view bytes, size_t offset) {
    return static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset]) |
                                 static_cast<unsigned char>(bytes[offset + 1]) << 8);
}

static uint32_t read32(std::string_view bytes, size_t offset) {
    return static_cast<uint32_t>(read16(bytes, offset)) | static_cast<uint32_t>(read16(bytes, offset + 2)) << 16;
}

// Whether an extra field holds zip64's sizes
static bool has_zip64_extra(std::string_view extra) {
    size_t offset = 0;
    while (offset + EXTRA_HEADER_SIZE <= extra.size()) {
        if (read16(extra, offset) == ZIP64_EXTRA_ID) {
            return true;
        }

        offset += EXTRA_HEADER_SIZE + read16(extra, offset + EXTRA_SIZE_OFFSET);
    }

    return false;
}

// Why mcufw cannot read an entry from its flags and sizes, or empty when it can
static std::string unreadable_form(uint16_t flags, const local_entry& entry, std::string_view extra) {
    if (flags & FLAG_ENCRYPTED) {
        return "is encrypted";
    }

    if (flags & FLAG_DATA_DESCRIPTOR) {
        return "states its sizes after its data";
    }

    if (entry.compressed_size == ZIP64_MARKER || entry.size == ZIP64_MARKER || has_zip64_extra(extra)) {
        return "is in zip64";
    }

    return {};
}

static bool begin_entry(zip_stream& stream, std::string& error) {
    std::string_view header = stream.record;
    uint16_t flags = read16(header, LOCAL_FLAGS_OFFSET);
    uint16_t name_size = read16(header, LOCAL_NAME_SIZE_OFFSET);
    local_entry entry = { std::string(header.substr(LOCAL_HEADER_SIZE, name_size)), read32(header, LOCAL_CRC_OFFSET),
                          read32(header, LOCAL_COMPRESSED_SIZE_OFFSET), read32(header, LOCAL_SIZE_OFFSET) };
    std::string_view extra = header.substr(LOCAL_HEADER_SIZE + name_size);

    stream.method = read16(header, LOCAL_METHOD_OFFSET);
    std::string form = unreadable_form(flags, entry, extra);
    if (!form.empty()) {
        error = entry.path + " " + form + ", which mcufw does not read";
        return false;
    }

    if (stream.method != METHOD_STORED && stream.method != METHOD_DEFLATED) {
        error = entry.path + " is compressed with method " + std::to_string(stream.method) + ", not stored or deflated";
        return false;
    }

    if (stream.method == METHOD_STORED && entry.compressed_size != entry.size) {
        error = entry.path + " is stored uncompressed but states two different sizes";
        return false;
    }

    if (entry.path.ends_with('/') && entry.size != 0) {
        error = entry.path + " is a folder that holds data";
        return false;
    }

    // Each entry writes no more than its stated size, so the stated sizes bound everything the archive unpacks
    if (entry.size > stream.unpacked_max - stream.unpacked_stated) {
        error = entry.path + " takes the archive past the " + std::to_string(stream.unpacked_max) +
                " bytes it may unpack to";
        return false;
    }

    stream.unpacked_stated += entry.size;

    if (!stream.paths.insert(entry.path).second) {
        error = "the archive names " + entry.path + " twice";
        return false;
    }

    if (stream.method == METHOD_DEFLATED) {
        int status = stream.inflater_ready ? inflateReset(&stream.inflater)
                                           : inflateInit2(&stream.inflater, -MAX_WBITS);
        if (status != Z_OK) {
            error = "inflate cannot start";
            return false;
        }

        stream.inflater_ready = true;
    }

    stream.entry = entry;
    stream.remaining = entry.compressed_size;
    stream.written = 0;
    stream.crc = crc32(0, nullptr, 0);
    stream.inflate_ended = false;

    return stream.handlers.begin_entry({ entry.path, entry.size }, error);
}

static bool take_output(zip_stream& stream, std::string_view bytes, std::string& error) {
    stream.written += bytes.size();
    if (stream.written > stream.entry.size) {
        error = stream.entry.path + " holds more than the " + std::to_string(stream.entry.size) + " bytes it states";
        return false;
    }

    stream.crc = crc32(stream.crc, reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uInt>(bytes.size()));

    return stream.handlers.entry_bytes(bytes, error);
}

static bool inflate_bytes(zip_stream& stream, std::string_view bytes, std::string& error) {
    auto consume = [&stream](std::string_view output, std::string& consume_error) {
        return take_output(stream, output, consume_error);
    };

    inflate_failure why = inflate_failure::consumer;
    stream.output.resize(INFLATE_OUTPUT_BYTES);
    if (inflate_input(stream.inflater, bytes, stream.output, stream.inflate_ended, consume, why, error)) {
        return true;
    }

    if (why == inflate_failure::corrupt) {
        error = stream.entry.path + " holds deflated data that is corrupt";
    } else if (why == inflate_failure::after_end) {
        error = stream.entry.path + " holds bytes after its deflated data";
    }

    return false;
}

static bool end_entry(zip_stream& stream, std::string& error) {
    const local_entry& entry = stream.entry;
    if (stream.method == METHOD_DEFLATED && !stream.inflate_ended) {
        error = entry.path + " holds deflated data that is cut short";
        return false;
    }

    if (stream.written != entry.size || stream.crc != entry.crc) {
        error = entry.path + " does not match the size and CRC-32 its header states";
        return false;
    }

    stream.entries.push_back(entry);
    stream.part = zip_part::signature;

    return stream.handlers.end_entry(error);
}

static bool check_central_entry(zip_stream& stream, std::string& error) {
    std::string_view header = stream.record;
    uint16_t name_size = read16(header, CENTRAL_NAME_SIZE_OFFSET);
    std::string path(header.substr(CENTRAL_HEADER_SIZE, name_size));
    size_t index = stream.members.size();
    bool matches = index < stream.entries.size() && stream.entries[index].path == path &&
                   stream.entries[index].crc == read32(header, CENTRAL_CRC_OFFSET) &&
                   stream.entries[index].compressed_size == read32(header, CENTRAL_COMPRESSED_SIZE_OFFSET) &&
                   stream.entries[index].size == read32(header, CENTRAL_SIZE_OFFSET);
    if (!matches) {
        error = "the central directory disagrees with the entries about " + path;
        return false;
    }

    bool made_on_unix = read16(header, CENTRAL_MADE_BY_OFFSET) >> MADE_BY_HOST_SHIFT == HOST_UNIX;
    uint32_t attributes = read32(header, CENTRAL_EXTERNAL_ATTRIBUTES_OFFSET);
    stream.members.push_back({ path, made_on_unix ? attributes >> UNIX_MODE_SHIFT : 0 });
    stream.part = zip_part::signature;

    return true;
}

static bool check_end_record(zip_stream& stream, std::string& error) {
    std::string_view record = stream.record;
    uint16_t total = read16(record, END_TOTAL_ENTRIES_OFFSET);
    if (total == ZIP64_ENTRIES_MARKER) {
        error = "the archive is zip64, which mcufw does not read";
        return false;
    }

    if (total != stream.entries.size() || total != stream.members.size()) {
        error = "the central directory and the archive hold different numbers of entries";
        return false;
    }

    stream.remaining = read16(record, END_COMMENT_SIZE_OFFSET);
    stream.part = stream.remaining > 0 ? zip_part::end_comment : zip_part::done;

    return true;
}

static bool start_record(zip_stream& stream, std::string& error) {
    uint32_t signature = read32(stream.record, 0);
    if (signature == LOCAL_HEADER_SIGNATURE && !stream.in_central) {
        stream.part = zip_part::local_header;
        stream.record_size = LOCAL_HEADER_SIZE;
        return true;
    }

    if (signature == CENTRAL_HEADER_SIGNATURE) {
        stream.in_central = true;
        stream.part = zip_part::central_header;
        stream.record_size = CENTRAL_HEADER_SIZE;
        return true;
    }

    if (signature == END_RECORD_SIGNATURE) {
        stream.part = zip_part::end_record;
        stream.record_size = END_RECORD_SIZE;
        return true;
    }

    bool is_zip64 = signature == ZIP64_END_RECORD_SIGNATURE || signature == ZIP64_LOCATOR_SIGNATURE;
    error = is_zip64 ? "the archive is zip64, which mcufw does not read" : "the archive is no zip, or is corrupt";

    return false;
}

// Adds bytes to the record being read, learning its whole size from its fixed part
static size_t gather_record(zip_stream& stream, std::string_view bytes) {
    size_t taken = std::min(bytes.size(), stream.record_size - stream.record.size());
    stream.record.append(bytes.substr(0, taken));
    if (stream.record.size() != stream.record_size) {
        return taken;
    }

    std::string_view record = stream.record;
    if (stream.part == zip_part::local_header && stream.record_size == LOCAL_HEADER_SIZE) {
        stream.record_size += read16(record, LOCAL_NAME_SIZE_OFFSET) + read16(record, LOCAL_EXTRA_SIZE_OFFSET);
    } else if (stream.part == zip_part::central_header && stream.record_size == CENTRAL_HEADER_SIZE) {
        stream.record_size += read16(record, CENTRAL_NAME_SIZE_OFFSET) + read16(record, CENTRAL_EXTRA_SIZE_OFFSET) +
                              read16(record, CENTRAL_COMMENT_SIZE_OFFSET);
    }

    return taken;
}

static bool feed_stream(zip_stream& stream, std::string_view bytes, std::string& error) {
    while (!bytes.empty()) {
        if (stream.part == zip_part::done) {
            error = "the archive holds bytes after its end";
            return false;
        }

        if (stream.part == zip_part::entry_data || stream.part == zip_part::end_comment) {
            size_t taken = static_cast<size_t>(std::min<uint64_t>(bytes.size(), stream.remaining));
            std::string_view chunk = bytes.substr(0, taken);
            bytes.remove_prefix(taken);
            stream.remaining -= taken;
            if (stream.part == zip_part::end_comment) {
                stream.part = stream.remaining == 0 ? zip_part::done : zip_part::end_comment;
                continue;
            }

            bool fed = stream.method == METHOD_STORED ? take_output(stream, chunk, error)
                                                      : inflate_bytes(stream, chunk, error);
            if (!fed || (stream.remaining == 0 && !end_entry(stream, error))) {
                return false;
            }

            continue;
        }

        if (stream.part == zip_part::signature && stream.record.empty()) {
            stream.record_size = SIGNATURE_SIZE;
        }

        bytes.remove_prefix(gather_record(stream, bytes));
        if (stream.record.size() < stream.record_size) {
            continue;
        }

        if (stream.part == zip_part::signature) {
            if (!start_record(stream, error)) {
                return false;
            }

            continue;
        }

        bool checked = true;
        zip_part part = stream.part;
        if (part == zip_part::local_header) {
            checked = begin_entry(stream, error);
            stream.part = zip_part::entry_data;
        } else if (part == zip_part::central_header) {
            checked = check_central_entry(stream, error);
        } else {
            checked = check_end_record(stream, error);
        }

        stream.record.clear();
        if (!checked) {
            return false;
        }

        if (part == zip_part::local_header && stream.remaining == 0 && !end_entry(stream, error)) {
            return false;
        }
    }

    return true;
}

zip_reader::zip_reader(zip_handlers handlers, uint64_t unpacked_max) : m_stream(std::make_unique<zip_stream>()) {
    m_stream->handlers = std::move(handlers);
    m_stream->unpacked_max = unpacked_max;
}

zip_reader::~zip_reader() {
    if (m_stream->inflater_ready) {
        inflateEnd(&m_stream->inflater);
    }
}

bool zip_reader::feed(std::string_view bytes, std::string& error) {
    return feed_stream(*m_stream, bytes, error);
}

bool zip_reader::finish(std::string& error) {
    if (m_stream->part != zip_part::done) {
        error = "the archive ends before its central directory does";
        return false;
    }

    return true;
}

const std::vector<zip_member>& zip_reader::members() const {
    return m_stream->members;
}
