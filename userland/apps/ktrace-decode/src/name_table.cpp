#include "name_table.hpp"

#include <stlx/ktrace_format.h>

#include <cstring>

constexpr const char* NAMES_SCHEMA = R"sql(
CREATE TABLE names (
    name_id           INTEGER PRIMARY KEY,
    name              TEXT NOT NULL      -- A task name as the kernel copied it, at most 16 bytes
);
)sql";

constexpr const char* INSERT_NAME_SQL = "INSERT INTO names (name_id, name) VALUES (?, ?)";

int64_t intern_task_name(name_ids& ids, const uint8_t* bytes) {
    const char* text = reinterpret_cast<const char*>(bytes);
    std::string name(text, strnlen(text, ktrace::TASK_NAME_BYTES));

    auto [entry, inserted] = ids.id_by_name.try_emplace(name, static_cast<int64_t>(ids.names.size() + 1));
    if (inserted) {
        ids.names.push_back(std::move(name));
    }

    return entry->second;
}

bool write_name_table(database& db, const name_ids& ids, std::string& error) {
    if (!execute_sql(db, NAMES_SCHEMA, error)) {
        return false;
    }

    sqlite_statement insert = prepare_statement(db, INSERT_NAME_SQL, error);
    if (!insert) {
        return false;
    }

    sqlite3_stmt* row = insert.get();
    for (size_t i = 0; i < ids.names.size(); i++) {
        sqlite3_bind_int64(row, 1, static_cast<int64_t>(i + 1));
        sqlite3_bind_text(row, 2, ids.names[i].data(), static_cast<int>(ids.names[i].size()), SQLITE_STATIC);
        if (!insert_row(db, row, error)) {
            return false;
        }
    }

    return true;
}
