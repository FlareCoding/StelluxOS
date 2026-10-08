#ifndef KTRACE_DECODE_SESSION_TABLES_HPP
#define KTRACE_DECODE_SESSION_TABLES_HPP

#include "database.hpp"
#include "health.hpp"
#include "trace_file.hpp"

#include <string>
#include <vector>

// Writes the session, cpus and health tables, which describe the recording as a whole
bool write_session_tables(database& db, const trace_file& file, const record_counts& counts,
                          const std::vector<health_check>& checks, std::string& error);

#endif // KTRACE_DECODE_SESSION_TABLES_HPP
