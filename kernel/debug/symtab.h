#ifndef STELLUX_DEBUG_SYMTAB_H
#define STELLUX_DEBUG_SYMTAB_H

#include "common/types.h"

namespace symtab {

constexpr int32_t OK            = 0;
constexpr int32_t ERR_NO_FILE   = -1;
constexpr int32_t ERR_BAD_ELF   = -2;
constexpr int32_t ERR_NO_SYMTAB = -3;
constexpr int32_t ERR_NO_MEMORY = -4;

struct resolve_result {
    const char* name;
    uint64_t    offset;
};

/**
 * @brief Parse kernel ELF and copy .symtab/.strtab into heap memory.
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Resolve a code address to its containing function symbol.
 * @param addr  Virtual address to resolve.
 * @param out   Result: symbol name and byte offset from symbol start.
 * @return true if a symbol was found, false otherwise.
 */
bool resolve(uint64_t addr, resolve_result* out);

} // namespace symtab

#endif // STELLUX_DEBUG_SYMTAB_H
