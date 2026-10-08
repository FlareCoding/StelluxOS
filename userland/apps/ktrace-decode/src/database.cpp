#include "database.hpp"
#include "text.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

constexpr const char* PARTIAL_SUFFIX = ".partial";

// The database is written once under a temporary name, so journaling, syncing
// and locking would only cost time
constexpr const char* BUILD_PRAGMAS =
    "PRAGMA journal_mode = OFF;"
    "PRAGMA synchronous = OFF;"
    "PRAGMA locking_mode = EXCLUSIVE;"
    "PRAGMA temp_store = MEMORY;";

static std::string describe_error(database& db) {
    return format_string("%s: %s", db.partial_path.c_str(), sqlite3_errmsg(db.connection.get()));
}

bool open_database(const char* path, database& db, std::string& error) {
    db.path = path;
    db.partial_path = db.path + PARTIAL_SUFFIX;

    if (unlink(db.partial_path.c_str()) != 0 && errno != ENOENT) {
        error = format_string("cannot remove the earlier %s: %s", db.partial_path.c_str(), strerror(errno));
        return false;
    }

    sqlite3* connection = nullptr;
    int result = sqlite3_open_v2(db.partial_path.c_str(), &connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                 nullptr);
    db.connection.reset(connection);
    if (result != SQLITE_OK) {
        int system_errno = connection ? sqlite3_system_errno(connection) : 0;
        error = format_string("cannot create %s: %s", db.partial_path.c_str(),
                              system_errno != 0 ? strerror(system_errno) : sqlite3_errstr(result));
        return false;
    }

    if (!execute_sql(db, BUILD_PRAGMAS, error)) {
        return false;
    }

    return execute_sql(db, "BEGIN", error);
}

bool execute_sql(database& db, const char* sql, std::string& error) {
    char* message = nullptr;
    if (sqlite3_exec(db.connection.get(), sql, nullptr, nullptr, &message) != SQLITE_OK) {
        error = format_string("%s: %s", db.partial_path.c_str(), message ? message : "unknown error");
        sqlite3_free(message);
        return false;
    }

    return true;
}

sqlite_statement prepare_statement(database& db, const char* sql, std::string& error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.connection.get(), sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = describe_error(db);
    }

    return sqlite_statement(statement, sqlite3_finalize);
}

bool insert_row(database& db, sqlite3_stmt* statement, std::string& error) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        error = describe_error(db);
        sqlite3_reset(statement);
        return false;
    }

    sqlite3_reset(statement);
    return true;
}

bool finish_database(database& db, std::string& error) {
    if (!execute_sql(db, "COMMIT", error)) {
        return false;
    }

    int result = sqlite3_close(db.connection.get());
    if (result != SQLITE_OK) {
        error = format_string("cannot close %s: %s", db.partial_path.c_str(), sqlite3_errstr(result));
        return false;
    }

    db.connection.release();

    if (rename(db.partial_path.c_str(), db.path.c_str()) != 0) {
        error = format_string("cannot rename %s to %s: %s", db.partial_path.c_str(), db.path.c_str(),
                              strerror(errno));
        return false;
    }

    return true;
}

bool discard_database(database& db, std::string& error) {
    db.connection.reset();

    if (unlink(db.partial_path.c_str()) != 0 && errno != ENOENT) {
        error = format_string("cannot remove %s: %s", db.partial_path.c_str(), strerror(errno));
        return false;
    }

    return true;
}
