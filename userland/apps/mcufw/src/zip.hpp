#ifndef MCUFW_ZIP_HPP
#define MCUFW_ZIP_HPP

// A reader for zip archives fed bytes as they arrive, so an archive unpacks as it downloads and is never held whole.
// Entries are checked against their CRC-32 and the central directory, which must list exactly the entries read.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct zip_entry {
    std::string path;  // as the archive names it, a directory's ending in a slash
    uint64_t    size;  // uncompressed bytes
};

// The reader calls these for each entry in archive order. `entry_bytes` gets bytes before their size and CRC-32 are
// checked, and `end_entry` runs only once they match.
struct zip_handlers {
    std::function<bool(const zip_entry& entry, std::string& error)> begin_entry;
    std::function<bool(std::string_view bytes, std::string& error)> entry_bytes;
    std::function<bool(std::string& error)>                         end_entry;
};

// An entry as the central directory states it
struct zip_member {
    std::string path;
    uint32_t    unix_mode;  // 0 when the archive was not made on Unix
};

class zip_reader {
public:
    // Refuses an entry whose stated size takes the entries' total past `unpacked_max` bytes, before it writes any
    zip_reader(zip_handlers handlers, uint64_t unpacked_max);
    ~zip_reader();

    // Fails on an entry that is encrypted, sized after its data, in zip64, compressed other than stored or deflated,
    // corrupt or named twice, a folder holding data, a central directory that disagrees, or a handler's failure
    bool feed(std::string_view bytes, std::string& error);

    // Fails unless the archive ended with a central directory matching every entry read
    bool finish(std::string& error);

    // Each entry with its mode, in archive order, once finish succeeded
    const std::vector<zip_member>& members() const;

    struct zip_stream;  // the parse state and zlib's stream, defined in zip.cpp

private:
    std::unique_ptr<zip_stream> m_stream;
};

#endif // MCUFW_ZIP_HPP
