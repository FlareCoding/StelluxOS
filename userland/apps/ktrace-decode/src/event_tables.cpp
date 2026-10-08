#include "event_tables.hpp"
#include "text.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

struct column_definition {
    std::string name;
    const char* note;
};

struct event_table {
    const ktrace::event_description* event;
    sqlite_statement                 insert;
};

constexpr const char* UNKNOWN_RECORDS_SCHEMA = R"sql(
CREATE TABLE unknown_records (
    ts_ns             INTEGER NOT NULL,  -- When the kernel wrote the record, nanoseconds since boot
    cpu               INTEGER NOT NULL,  -- The CPU whose ring held the record
    event_id          INTEGER NOT NULL,
    record            BLOB NOT NULL      -- The whole 64-byte record, header included
);
)sql";

constexpr const char* INSERT_UNKNOWN_RECORD_SQL =
    "INSERT INTO unknown_records (ts_ns, cpu, event_id, record) VALUES (?, ?, ?, ?)";

constexpr const char* TS_NS_NOTE   = "When the kernel wrote the record, nanoseconds since boot";
constexpr const char* CPU_NOTE     = "The CPU whose ring held the record";
constexpr const char* NAME_ID_NOTE = "A names.name_id";

// The payload's fields bind from this column on, after ts_ns and cpu
constexpr int FIRST_FIELD_COLUMN = 3;

static std::vector<column_definition> event_columns(const ktrace::event_description& event) {
    std::vector<column_definition> columns = {{"ts_ns", TS_NS_NOTE}, {"cpu", CPU_NOTE}};
    for (size_t i = 0; i < event.field_count; i++) {
        const ktrace::event_field& field = event.fields[i];
        if (field.type == ktrace::FIELD_RESERVED) {
            continue;
        }

        if (field.type == ktrace::FIELD_TASK_NAME) {
            columns.push_back({std::string(field.name) + "_id", NAME_ID_NOTE});
        } else {
            columns.push_back({field.name, nullptr});
        }
    }

    return columns;
}

static std::string create_table_sql(const char* table, const std::vector<column_definition>& columns) {
    std::string sql = format_string("CREATE TABLE %s (\n", table);
    for (size_t i = 0; i < columns.size(); i++) {
        bool is_last = i + 1 == columns.size();
        sql += format_string("    %-17s INTEGER NOT NULL%s", columns[i].name.c_str(), is_last ? "" : ",");
        if (columns[i].note) {
            sql += format_string("%s  -- %s", is_last ? " " : "", columns[i].note);
        }

        sql += "\n";
    }

    sql += ");";
    return sql;
}

static std::string insert_sql(const char* table, const std::vector<column_definition>& columns) {
    std::string column_list;
    std::string placeholders;
    for (const column_definition& column : columns) {
        if (!column_list.empty()) {
            column_list += ", ";
            placeholders += ", ";
        }

        column_list += column.name;
        placeholders += "?";
    }

    return format_string("INSERT INTO %s (%s) VALUES (%s)", table, column_list.c_str(), placeholders.c_str());
}

static bool create_event_tables(database& db, std::vector<event_table>& tables, std::string& error) {
    for (const ktrace::event_description& event : ktrace::EVENTS) {
        std::vector<column_definition> columns = event_columns(event);
        if (!execute_sql(db, create_table_sql(event.name, columns).c_str(), error)) {
            return false;
        }

        sqlite_statement insert = prepare_statement(db, insert_sql(event.name, columns).c_str(), error);
        if (!insert) {
            return false;
        }

        tables.push_back({&event, std::move(insert)});
    }

    return true;
}

template <typename T>
static T read_field(const uint8_t* payload, const ktrace::event_field& field) {
    T value;
    memcpy(&value, payload + field.offset, sizeof(value));
    return value;
}

static void bind_payload_fields(sqlite3_stmt* row, const ktrace::event_description& event, const uint8_t* payload,
                                name_ids& ids) {
    int column = FIRST_FIELD_COLUMN;
    for (size_t i = 0; i < event.field_count; i++) {
        const ktrace::event_field& field = event.fields[i];
        if (field.type == ktrace::FIELD_RESERVED) {
            continue;
        }

        switch (field.type) {
        case ktrace::FIELD_U8:
            sqlite3_bind_int64(row, column, read_field<uint8_t>(payload, field));
            break;
        case ktrace::FIELD_U32:
            sqlite3_bind_int64(row, column, read_field<uint32_t>(payload, field));
            break;
        case ktrace::FIELD_U64:
            bind_u64(row, column, read_field<uint64_t>(payload, field));
            break;
        case ktrace::FIELD_I32:
            sqlite3_bind_int64(row, column, read_field<int32_t>(payload, field));
            break;
        case ktrace::FIELD_I64:
            sqlite3_bind_int64(row, column, read_field<int64_t>(payload, field));
            break;
        case ktrace::FIELD_TASK_NAME:
            sqlite3_bind_int64(row, column, intern_task_name(ids, payload + field.offset));
            break;
        }

        column++;
    }
}

static bool write_record_rows(database& db, const trace_file& file, const timeline& order,
                              std::vector<event_table>& tables, name_ids& ids, std::string& error) {
    event_table* table_by_event_id[ktrace::EVENT_ID_COUNT] = {};
    for (event_table& table : tables) {
        table_by_event_id[table.event->id] = &table;
    }

    sqlite_statement insert_unknown = prepare_statement(db, INSERT_UNKNOWN_RECORD_SQL, error);
    if (!insert_unknown) {
        return false;
    }

    for (const timeline_record& record : order.records) {
        const uint8_t* bytes = record_bytes(file, record);
        uint16_t event_id = record_event_id(bytes);

        event_table* table = event_id < ktrace::EVENT_ID_COUNT ? table_by_event_id[event_id] : nullptr;
        sqlite3_stmt* row = table ? table->insert.get() : insert_unknown.get();
        bind_u64(row, 1, record.ts_ns);
        sqlite3_bind_int64(row, 2, record.cpu);

        if (table) {
            bind_payload_fields(row, *table->event, bytes + sizeof(ktrace::trace_record_header), ids);
        } else {
            sqlite3_bind_int64(row, 3, event_id);
            sqlite3_bind_blob(row, 4, bytes, sizeof(ktrace::trace_record), SQLITE_STATIC);
        }

        if (!insert_row(db, row, error)) {
            return false;
        }
    }

    return true;
}

// Analyses select an event's records by thread, then by time. Every field
// holding a thread id is named with a tid suffix.
static bool create_thread_indexes(database& db, std::string& error) {
    for (const ktrace::event_description& event : ktrace::EVENTS) {
        for (size_t i = 0; i < event.field_count; i++) {
            std::string_view field = event.fields[i].name;
            if (!field.ends_with("tid")) {
                continue;
            }

            std::string sql = format_string("CREATE INDEX %s_by_%s ON %s (%s, ts_ns)", event.name,
                                            event.fields[i].name, event.name, event.fields[i].name);
            if (!execute_sql(db, sql.c_str(), error)) {
                return false;
            }
        }
    }

    return true;
}

bool write_event_tables(database& db, const trace_file& file, const timeline& order, name_ids& ids,
                        std::string& error) {
    if (!execute_sql(db, UNKNOWN_RECORDS_SCHEMA, error)) {
        return false;
    }

    std::vector<event_table> tables;
    if (!create_event_tables(db, tables, error)) {
        return false;
    }

    if (!write_record_rows(db, file, order, tables, ids, error)) {
        return false;
    }

    return create_thread_indexes(db, error);
}
