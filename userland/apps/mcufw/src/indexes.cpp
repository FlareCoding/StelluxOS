#include "indexes.hpp"

#include "inflate.hpp"
#include "json.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

constexpr size_t GZIP_CHUNK_BYTES = 64 * 1024;

// The library index inflates to 61 MB and the package index to 1.7 MB, so this holds four times the larger
constexpr uint64_t INDEX_INFLATED_MAX = 256 << 20;

// The package index lists 14 packages with 331 tools, so these hold more than ten times as many
constexpr size_t INDEX_PACKAGES_MAX = 256;
constexpr size_t INDEX_TOOLS_MAX = 4096;
constexpr int GZIP_WINDOW_BITS = 16 + MAX_WBITS;
constexpr const char* DATA_HOST = "all";
// What a Linux host's name holds, as in `x86_64-linux-gnu` and `arm-linux-gnueabihf`
constexpr std::string_view LINUX_HOST_PART = "-linux-gnu";

static bool inflate_from(int fd, const std::string& path, z_stream& stream, const inflated_consumer& consume,
                         std::string& error) {
    uint64_t inflated = 0;
    auto bounded = [&](std::string_view bytes, std::string& consume_error) {
        inflated += bytes.size();
        if (inflated > INDEX_INFLATED_MAX) {
            consume_error = path + " inflates to more than " + std::to_string(INDEX_INFLATED_MAX >> 20) + " MiB";
            return false;
        }

        return consume(bytes, consume_error);
    };

    std::vector<unsigned char> input(GZIP_CHUNK_BYTES);
    std::vector<unsigned char> output(GZIP_CHUNK_BYTES);
    bool ended = false;
    while (!ended) {
        ssize_t count = read(fd, input.data(), input.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count <= 0) {
            error = count < 0 ? "cannot read " + path + ": " + strerror(errno) : path + " is cut short";
            return false;
        }

        inflate_failure why = inflate_failure::consumer;
        std::string_view piece(reinterpret_cast<const char*>(input.data()), static_cast<size_t>(count));
        if (!inflate_input(stream, piece, output, ended, bounded, why, error)) {
            if (why == inflate_failure::corrupt) {
                error = path + " is corrupt";
            } else if (why == inflate_failure::after_end) {
                error = path + " holds bytes after its gzip data";
            }

            return false;
        }
    }

    return true;
}

static bool read_gzip_file(const std::string& path, const inflated_consumer& consume, std::string& error) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path + ": " + strerror(errno);
        return false;
    }

    z_stream stream{};
    if (inflateInit2(&stream, GZIP_WINDOW_BITS) != Z_OK) {
        close(fd);
        error = "inflate cannot start";
        return false;
    }

    bool inflated = inflate_from(fd, path, stream, consume, error);
    inflateEnd(&stream);
    close(fd);

    return inflated;
}

static std::string member_text(const json_value& object, std::string_view name) {
    const json_value* member = json_member(object, name);
    return member && member->kind == json_kind::string ? member->text : std::string();
}

static const std::vector<json_value>& member_items(const json_value& object, std::string_view name) {
    static const std::vector<json_value> none;
    const json_value* member = json_member(object, name);
    return member && member->kind == json_kind::array ? member->items : none;
}

// The index writes a size as a string of digits, or for some tools as a number, and a size that is missing, holds
// other text or passes 64 bits reads as 0
static uint64_t member_size(const json_value& object) {
    const json_value* member = json_member(object, "size");
    bool is_stated = member && (member->kind == json_kind::string || member->kind == json_kind::number);
    std::string text = is_stated ? member->text : std::string();
    uint64_t size = 0;
    for (char c : text) {
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (c < '0' || c > '9' || size > (UINT64_MAX - digit) / 10) {
            return 0;
        }

        size = size * 10 + digit;
    }

    return size;
}

static archive_download read_archive(const json_value& object) {
    return { member_text(object, "url"), member_text(object, "archiveFileName"), member_text(object, "checksum"),
             member_size(object) };
}

static tool_release read_tool(const std::string& packager, const json_value& tool) {
    tool_release release = { packager, member_text(tool, "name"), member_text(tool, "version"), false, {} };
    const std::vector<json_value>& systems = member_items(tool, "systems");
    for (const json_value& system : systems) {
        if (member_text(system, "host") == DATA_HOST) {
            release.is_data = true;
            release.archive = read_archive(system);
            return release;
        }
    }

    bool shares_one_archive = systems.size() >= 2;
    bool has_linux_host = false;
    for (const json_value& system : systems) {
        std::string checksum = member_text(system, "checksum");
        shares_one_archive = shares_one_archive && !checksum.empty() && checksum == member_text(systems[0], "checksum");
        has_linux_host = has_linux_host || member_text(system, "host").find(LINUX_HOST_PART) != std::string::npos;
    }

    if (shares_one_archive && has_linux_host) {
        release.is_data = true;
        release.archive = read_archive(systems[0]);
    }

    return release;
}

static bool read_package(const json_value& package, const std::string& packager, const std::string& architecture,
                         std::vector<core_release>& cores, std::vector<tool_release>& tools, std::string& error) {
    std::string name = member_text(package, "name");
    for (const json_value& platform : member_items(package, "platforms")) {
        if (name != packager || member_text(platform, "architecture") != architecture) {
            continue;
        }

        core_release release = { name, architecture, member_text(platform, "version"), member_text(platform, "name"),
                                 read_archive(platform), {} };
        for (const json_value& dependency : member_items(platform, "toolsDependencies")) {
            release.tools.push_back({ member_text(dependency, "packager"), member_text(dependency, "name"),
                                      member_text(dependency, "version") });
        }

        cores.push_back(std::move(release));
    }

    for (const json_value& tool : member_items(package, "tools")) {
        tools.push_back(read_tool(name, tool));
    }

    if (tools.size() > INDEX_TOOLS_MAX) {
        error = "the index lists more than " + std::to_string(INDEX_TOOLS_MAX) + " tools";
        return false;
    }

    return true;
}

bool read_package_index(const std::string& path, const std::string& packager, const std::string& architecture,
                        std::vector<core_release>& cores, std::vector<tool_release>& tools, std::string& error) {
    cores.clear();
    tools.clear();
    size_t packages = 0;
    json_array_reader reader("packages", [&](json_value& package, std::string& take_error) {
        if (++packages > INDEX_PACKAGES_MAX) {
            take_error = "the index lists more than " + std::to_string(INDEX_PACKAGES_MAX) + " packages";
            return false;
        }

        return read_package(package, packager, architecture, cores, tools, take_error);
    });

    auto feed = [&](std::string_view bytes, std::string& feed_error) {
        return reader.feed(bytes, feed_error);
    };

    if (!read_gzip_file(path, feed, error) || !reader.finish(error)) {
        error = "cannot read the package index, since " + error;
        return false;
    }

    return true;
}
