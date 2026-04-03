#ifndef STELLUX_ARCH_AARCH64_EXEC_ELF_ARCH_H
#define STELLUX_ARCH_AARCH64_EXEC_ELF_ARCH_H

#include "exec/elf64.h"

namespace exec {
constexpr uint16_t ELF_EXPECTED_MACHINE = elf64::EM_AARCH64;
} // namespace exec

#endif // STELLUX_ARCH_AARCH64_EXEC_ELF_ARCH_H
