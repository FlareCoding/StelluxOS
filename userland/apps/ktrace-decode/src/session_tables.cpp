#include "session_tables.hpp"

constexpr const char* SESSION_SCHEMA = R"sql(
CREATE TABLE session (
    format_version        INTEGER NOT NULL,
    arch                  TEXT,              -- NULL for an ELF machine this decoder has no name for
    elf_machine           INTEGER NOT NULL,
    cpu_count             INTEGER NOT NULL,
    boot_unix_ns          INTEGER,           -- Wall-clock time at boot, NULL without a real-time clock
    start_ns              INTEGER NOT NULL,  -- Nanoseconds since boot
    stop_ns               INTEGER,           -- Nanoseconds since boot, NULL when the file was cut short
    event_mask            INTEGER NOT NULL,  -- Bit n is set when the session recorded event id n, as signed 64 bits
    records               INTEGER NOT NULL,
    records_before_start  INTEGER NOT NULL,  -- Older than start_ns, so left out as the format says
    lost_records          INTEGER NOT NULL,  -- Dropped while a ring was full
    file_bytes            INTEGER NOT NULL
);

CREATE TABLE cpus (
    cpu                   INTEGER PRIMARY KEY,
    records               INTEGER NOT NULL,
    records_before_start  INTEGER NOT NULL,
    lost_records          INTEGER NOT NULL
);

CREATE TABLE health (
    name                  TEXT PRIMARY KEY,
    passed                INTEGER NOT NULL,  -- 1 or 0
    detail                TEXT NOT NULL
);
)sql";

constexpr const char* INSERT_SESSION_SQL =
    "INSERT INTO session (format_version, arch, elf_machine, cpu_count, boot_unix_ns, start_ns, stop_ns, "
    "event_mask, records, records_before_start, lost_records, file_bytes) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

constexpr const char* INSERT_CPU_SQL =
    "INSERT INTO cpus (cpu, records, records_before_start, lost_records) VALUES (?, ?, ?, ?)";

constexpr const char* INSERT_HEALTH_SQL =
    "INSERT INTO health (name, passed, detail) VALUES (?, ?, ?)";

static bool write_session_row(database& db, const trace_file& file, const record_counts& counts,
                              std::string& error) {
    sqlite_statement insert = prepare_statement(db, INSERT_SESSION_SQL, error);
    if (!insert) {
        return false;
    }

    const ktrace::file_header& header = file.header;
    sqlite3_stmt* row = insert.get();
    sqlite3_bind_int(row, 1, header.version);
    bind_text_or_null(row, 2, arch_name(header.arch));
    sqlite3_bind_int(row, 3, header.arch);
    sqlite3_bind_int64(row, 4, header.cpu_count);
    bind_u64_or_null(row, 5, header.boot_unix_ns, header.boot_unix_ns != 0);
    bind_u64(row, 6, header.session_start_ns);
    bind_u64_or_null(row, 7, file.stop_ns, file.has_end_chunk);
    bind_u64(row, 8, header.event_mask);

    bind_u64(row, 9, counts.in_session);
    bind_u64(row, 10, counts.before_start);
    bind_u64(row, 11, file.lost_records);
    bind_u64(row, 12, file.size);

    return insert_row(db, row, error);
}

static bool write_cpu_rows(database& db, const trace_file& file, const record_counts& counts,
                           std::string& error) {
    sqlite_statement insert = prepare_statement(db, INSERT_CPU_SQL, error);
    if (!insert) {
        return false;
    }

    sqlite3_stmt* row = insert.get();
    for (uint32_t cpu = 0; cpu < file.header.cpu_count; cpu++) {
        sqlite3_bind_int64(row, 1, cpu);
        bind_u64(row, 2, counts.in_session_by_cpu[cpu]);
        bind_u64(row, 3, counts.before_start_by_cpu[cpu]);
        bind_u64(row, 4, file.lost_records_by_cpu[cpu]);
        if (!insert_row(db, row, error)) {
            return false;
        }
    }

    return true;
}

static bool write_health_rows(database& db, const std::vector<health_check>& checks, std::string& error) {
    sqlite_statement insert = prepare_statement(db, INSERT_HEALTH_SQL, error);
    if (!insert) {
        return false;
    }

    sqlite3_stmt* row = insert.get();
    for (const health_check& check : checks) {
        sqlite3_bind_text(row, 1, check.name, -1, SQLITE_STATIC);
        sqlite3_bind_int(row, 2, check.passed ? 1 : 0);
        sqlite3_bind_text(row, 3, check.detail.c_str(), -1, SQLITE_STATIC);
        if (!insert_row(db, row, error)) {
            return false;
        }
    }

    return true;
}

bool write_session_tables(database& db, const trace_file& file, const record_counts& counts,
                          const std::vector<health_check>& checks, std::string& error) {
    if (!execute_sql(db, SESSION_SCHEMA, error)) {
        return false;
    }

    if (!write_session_row(db, file, counts, error)) {
        return false;
    }

    if (!write_cpu_rows(db, file, counts, error)) {
        return false;
    }

    return write_health_rows(db, checks, error);
}
