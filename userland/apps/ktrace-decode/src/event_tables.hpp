#ifndef KTRACE_DECODE_EVENT_TABLES_HPP
#define KTRACE_DECODE_EVENT_TABLES_HPP

#include "database.hpp"
#include "trace_file.hpp"

#include <string>

// Writes one table per event the format describes, built from its field list, plus the
// unknown_records and names tables. Rows go in by timestamp.
bool write_event_tables(database& db, const trace_file& file, const timeline& order, std::string& error);

#endif // KTRACE_DECODE_EVENT_TABLES_HPP
