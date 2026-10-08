#include "trace_file.hpp"
#include "text.hpp"

#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

// Returns 0 once `size` bytes are read or the file ends, or the errno value that stopped it
static int read_fully(int fd, uint8_t* buffer, size_t size, size_t& bytes_read) {
    while (bytes_read < size) {
        ssize_t n = read(fd, buffer + bytes_read, size - bytes_read);
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n < 0) {
            return errno;
        }

        if (n == 0) {
            return 0;
        }

        bytes_read += static_cast<size_t>(n);
    }

    return 0;
}

static bool read_whole_file(const char* path, trace_file& file, std::string& error) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        error = format_string("cannot open %s: %s", path, strerror(errno));
        return false;
    }

    struct stat info;
    if (fstat(fd, &info) != 0) {
        error = format_string("cannot stat %s: %s", path, strerror(errno));
        close(fd);
        return false;
    }

    file.size = static_cast<size_t>(info.st_size);
    file.bytes = std::make_unique_for_overwrite<uint8_t[]>(file.size);

    size_t bytes_read = 0;
    int read_error = read_fully(fd, file.bytes.get(), file.size, bytes_read);
    close(fd);

    if (read_error != 0) {
        error = format_string("cannot read %s: %s", path, strerror(read_error));
        return false;
    }

    if (bytes_read < file.size) {
        error = format_string("%s shrank to %zu bytes while it was read", path, bytes_read);
        return false;
    }

    return true;
}

static bool validate_file_header(const char* path, trace_file& file, std::string& error) {
    if (file.size < sizeof(ktrace::file_header)) {
        error = format_string("%s is %zu bytes, too short for a .ktrace header", path, file.size);
        return false;
    }

    memcpy(&file.header, file.bytes.get(), sizeof(file.header));
    const ktrace::file_header& header = file.header;
    if (memcmp(header.magic, ktrace::FILE_MAGIC, sizeof(header.magic)) != 0) {
        error = format_string("%s is not a .ktrace file", path);
        return false;
    }

    if (header.version != ktrace::FILE_VERSION) {
        error = format_string("%s has format version %u, but this decoder reads version %u", path,
                              header.version, ktrace::FILE_VERSION);
        return false;
    }

    if (header.header_size < sizeof(ktrace::file_header) || header.header_size > file.size) {
        error = format_string("%s declares a %u-byte header, which does not fit its %zu bytes", path,
                              header.header_size, file.size);
        return false;
    }

    if (header.record_size != sizeof(ktrace::trace_record)) {
        error = format_string("%s declares %u-byte records, but this decoder reads %zu-byte records", path,
                              header.record_size, sizeof(ktrace::trace_record));
        return false;
    }

    if (header.cpu_count == 0) {
        error = format_string("%s declares no CPUs", path);
        return false;
    }

    return true;
}

static bool index_chunks(const char* path, trace_file& file, std::string& error) {
    file.lost_records_by_cpu.assign(file.header.cpu_count, 0);

    size_t offset = file.header.header_size;
    while (file.size - offset >= sizeof(ktrace::chunk_header)) {
        ktrace::chunk_header chunk;
        memcpy(&chunk, file.bytes.get() + offset, sizeof(chunk));
        size_t records_offset = offset + sizeof(chunk);

        if (chunk.kind == ktrace::CHUNK_END) {
            file.has_end_chunk = true;
            file.stop_ns = chunk.stop_ns;
            offset = records_offset;
            break;
        }

        if (chunk.kind != ktrace::CHUNK_RECORDS) {
            error = format_string("%s: the chunk at byte %zu has unknown kind %u", path, offset, chunk.kind);
            return false;
        }

        if (chunk.cpu_id >= file.header.cpu_count) {
            error = format_string("%s: the chunk at byte %zu is for CPU %u, but the session has %u CPUs", path,
                                  offset, chunk.cpu_id, file.header.cpu_count);
            return false;
        }

        // A file cut short keeps the whole records written before the cut
        uint64_t declared_records = chunk.record_count;
        uint64_t whole_records = (file.size - records_offset) / sizeof(ktrace::trace_record);
        uint64_t record_count = std::min(declared_records, whole_records);
        file.chunks.push_back({chunk.cpu_id, records_offset, record_count});
        file.lost_records_by_cpu[chunk.cpu_id] += chunk.lost_records;
        file.lost_records += chunk.lost_records;

        offset = records_offset + record_count * sizeof(ktrace::trace_record);
        if (record_count < declared_records) {
            break;
        }
    }

    file.trailing_bytes = file.size - offset;
    return true;
}

bool load_trace_file(const char* path, trace_file& file, std::string& error) {
    if (!read_whole_file(path, file, error)) {
        return false;
    }

    if (!validate_file_header(path, file, error)) {
        return false;
    }

    return index_chunks(path, file, error);
}

record_counts count_records(const trace_file& file) {
    record_counts counts;
    counts.in_session_by_cpu.assign(file.header.cpu_count, 0);
    counts.before_start_by_cpu.assign(file.header.cpu_count, 0);

    uint64_t session_start_ns = file.header.session_start_ns;
    for (const record_chunk& chunk : file.chunks) {
        const uint8_t* first_record = file.bytes.get() + chunk.first_record_offset;
        for (uint64_t i = 0; i < chunk.record_count; i++) {
            ktrace::trace_record_header record;
            memcpy(&record, first_record + i * sizeof(ktrace::trace_record), sizeof(record));

            if (record.timestamp < session_start_ns) {
                counts.before_start_by_cpu[chunk.cpu]++;
                counts.before_start++;
                continue;
            }

            counts.in_session_by_cpu[chunk.cpu]++;
            counts.in_session++;
            if (record.event_id < ktrace::EVENT_ID_COUNT) {
                counts.by_event_id[record.event_id]++;
            } else {
                counts.with_event_id_out_of_range++;
            }
        }
    }

    return counts;
}

const char* arch_name(uint16_t elf_machine) {
    switch (elf_machine) {
    case EM_X86_64:
        return "x86_64";
    case EM_AARCH64:
        return "aarch64";
    default:
        return nullptr;
    }
}
