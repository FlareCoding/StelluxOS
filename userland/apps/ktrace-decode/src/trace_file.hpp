#ifndef KTRACE_DECODE_TRACE_FILE_HPP
#define KTRACE_DECODE_TRACE_FILE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <stlx/ktrace_format.h>

// One CPU's records as a chunk streamed them, left in place in the file's bytes
struct record_chunk {
    uint32_t cpu;
    size_t   first_record_offset;
    uint64_t record_count;
};

struct trace_file {
    std::unique_ptr<uint8_t[]> bytes;
    size_t                     size = 0;
    ktrace::file_header        header = {};
    std::vector<record_chunk>  chunks;
    std::vector<uint64_t>      lost_records_by_cpu;
    uint64_t                   lost_records = 0;
    bool                       has_end_chunk = false;
    uint64_t                   stop_ns = 0;
    size_t                     trailing_bytes = 0;  // After the last whole chunk or record
};

struct record_counts {
    std::vector<uint64_t> in_session_by_cpu;
    std::vector<uint64_t> before_start_by_cpu;
    uint64_t              in_session = 0;
    uint64_t              before_start = 0;
    uint64_t              by_event_id[ktrace::EVENT_ID_COUNT] = {};
    uint64_t              with_event_id_out_of_range = 0;
};

// One record of the session, found by its index among the file's 64-byte blocks
struct timeline_record {
    uint64_t ts_ns;
    uint32_t block_index;
    uint32_t cpu;
};

struct timeline {
    std::vector<timeline_record> records;  // By timestamp, a tie going to the lower CPU
    record_counts                counts;
};

// Reads and validates the .ktrace file at `path` and indexes its chunks. A file
// cut short keeps every whole record before the cut.
bool load_trace_file(const char* path, trace_file& file, std::string& error);

// Orders the session's records by timestamp across CPUs, leaving out those older
// than its start as the format says
timeline build_timeline(const trace_file& file);

inline const uint8_t* record_bytes(const trace_file& file, const timeline_record& record) {
    return file.bytes.get() + static_cast<size_t>(record.block_index) * sizeof(ktrace::trace_record);
}

// Returns nullptr for an ELF machine number this decoder has no name for
const char* arch_name(uint16_t elf_machine);

// Returns nullptr for an event id the format does not describe
const ktrace::event_description* find_event_description(uint16_t event_id);

#endif // KTRACE_DECODE_TRACE_FILE_HPP
