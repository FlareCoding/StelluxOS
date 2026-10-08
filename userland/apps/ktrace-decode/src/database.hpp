#ifndef KTRACE_DECODE_DATABASE_HPP
#define KTRACE_DECODE_DATABASE_HPP

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string>

using sqlite_connection = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;
using sqlite_statement  = std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)>;

// A database built under a temporary name and renamed over `path` once complete
struct database {
    sqlite_connection connection{nullptr, sqlite3_close_v2};
    std::string       path;
    std::string       partial_path;
};

// Creates the partial database and begins the transaction that builds it
bool open_database(const char* path, database& db, std::string& error);

bool execute_sql(database& db, const char* sql, std::string& error);

// Returns an empty statement with `error` set when `sql` does not compile
sqlite_statement prepare_statement(database& db, const char* sql, std::string& error);

// Runs an insert once with its current bindings, then readies it for the next row
bool insert_row(database& db, sqlite3_stmt* statement, std::string& error);

// Commits and closes the database, then renames it over its path
bool finish_database(database& db, std::string& error);

// Closes the database and removes its partial file
bool discard_database(database& db, std::string& error);

// SQLite integers are signed, so values of 2^63 or more keep their bits and read back negative
inline void bind_u64(sqlite3_stmt* statement, int index, uint64_t value) {
    sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value));
}

inline void bind_u64_or_null(sqlite3_stmt* statement, int index, uint64_t value, bool has_value) {
    if (has_value) {
        bind_u64(statement, index, value);
    } else {
        sqlite3_bind_null(statement, index);
    }
}

inline void bind_text_or_null(sqlite3_stmt* statement, int index, const char* text) {
    if (text) {
        sqlite3_bind_text(statement, index, text, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(statement, index);
    }
}

#endif // KTRACE_DECODE_DATABASE_HPP
