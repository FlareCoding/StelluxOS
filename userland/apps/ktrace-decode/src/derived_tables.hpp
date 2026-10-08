#ifndef KTRACE_DECODE_DERIVED_TABLES_HPP
#define KTRACE_DECODE_DERIVED_TABLES_HPP

#include "database.hpp"
#include "name_table.hpp"
#include "trace_file.hpp"

#include <cstdint>
#include <string>

struct derived_counts {
    uint64_t cpu_slices = 0;
    uint64_t thread_states = 0;
    uint64_t threads = 0;
    uint64_t processes = 0;
};

// Writes what the records mean together: cpu_slices, thread_states, threads, thread_names and
// processes. Anything the records cannot determine is left out or NULL, never guessed.
bool write_derived_tables(database& db, const trace_file& file, const timeline& order, name_ids& ids,
                          derived_counts& counts, std::string& error);

#endif // KTRACE_DECODE_DERIVED_TABLES_HPP
