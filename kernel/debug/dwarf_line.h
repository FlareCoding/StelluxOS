#ifndef STELLUX_DEBUG_DWARF_LINE_H
#define STELLUX_DEBUG_DWARF_LINE_H

#include "common/types.h"
#include "debug/kernel_elf.h"

namespace dwarf_line {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_NO_SECTION  = -1;
constexpr int32_t ERR_NO_MEMORY   = -2;
constexpr int32_t ERR_BAD_DWARF   = -3;

struct resolve_result {
    const char* file;
    uint32_t    line;
};

/**
 * @brief Copy .debug_line and .debug_line_str from the kernel ELF into heap.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init(const debug::kernel_elf& elf);

/**
 * @brief Resolve an instruction address to file:line by walking the
 * DWARF5 line program on demand. Safe for use in panic paths.
 */
bool resolve(uint64_t addr, resolve_result* out);

} // namespace dwarf_line

#endif // STELLUX_DEBUG_DWARF_LINE_H
