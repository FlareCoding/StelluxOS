#ifndef KTRACE_DECODE_NAME_TABLE_HPP
#define KTRACE_DECODE_NAME_TABLE_HPP

#include "database.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Gives each distinct task name one id, numbered from 1 as the names first appear
struct name_ids {
    std::unordered_map<std::string, int64_t> id_by_name;
    std::vector<std::string>                 names;
};

// Returns the id of the name in the TASK_NAME_BYTES field at `bytes`
int64_t intern_task_name(name_ids& ids, const uint8_t* bytes);

// Writes the names table that every name id refers to
bool write_name_table(database& db, const name_ids& ids, std::string& error);

#endif // KTRACE_DECODE_NAME_TABLE_HPP
