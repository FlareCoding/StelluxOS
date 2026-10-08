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

// Reads and validates the .ktrace file at `path` and indexes its chunks. A file
// cut short keeps every whole record before the cut.
bool load_trace_file(const char* path, trace_file& file, std::string& error);

// Counts the session's records, leaving out those older than its start as the format says
record_counts count_records(const trace_file& file);

// Returns nullptr for an ELF machine number this decoder has no name for
const char* arch_name(uint16_t elf_machine);

#endif // KTRACE_DECODE_TRACE_FILE_HPP
