#ifndef STELLUX_ARCH_AARCH64_DEFS_EXCEPTION_H
#define STELLUX_ARCH_AARCH64_DEFS_EXCEPTION_H

#include "types.h"

namespace aarch64 {

// ESR_EL1 field masks and shifts
constexpr uint64_t ESR_EC_SHIFT = 26;
constexpr uint64_t ESR_EC_MASK  = 0x3F;        // bits [31:26]
constexpr uint64_t ESR_ISS_MASK = 0x00FFFFFF; // bits [24:0]

// Exception Class (EC) values - ARMv8-A (common ones)
constexpr uint8_t EC_UNKNOWN            = 0x00;
constexpr uint8_t EC_SVC_A64            = 0x15;
constexpr uint8_t EC_INST_ABORT_LOWER   = 0x20;
constexpr uint8_t EC_INST_ABORT_SAME    = 0x21;
constexpr uint8_t EC_PC_ALIGN           = 0x22;
constexpr uint8_t EC_DATA_ABORT_LOWER   = 0x24;
constexpr uint8_t EC_DATA_ABORT_SAME    = 0x25;
constexpr uint8_t EC_SP_ALIGN           = 0x26;
constexpr uint8_t EC_FP_A64             = 0x2C;
constexpr uint8_t EC_SERROR             = 0x2F;
constexpr uint8_t EC_BREAKPOINT_LOWER   = 0x30;
constexpr uint8_t EC_BREAKPOINT_SAME    = 0x31;
constexpr uint8_t EC_STEP_LOWER         = 0x32;
constexpr uint8_t EC_STEP_SAME          = 0x33;
constexpr uint8_t EC_WATCHPOINT_LOWER   = 0x34;
constexpr uint8_t EC_WATCHPOINT_SAME    = 0x35;
constexpr uint8_t EC_BRK_A64            = 0x3C;

// SPSR masks
constexpr uint64_t SPSR_MODE_MASK = 0x1F;
constexpr uint64_t SPSR_EL0T      = 0x00;  // EL0 using SP_EL0
constexpr uint64_t SPSR_EL1T      = 0x04;  // EL1 using SP_EL0
constexpr uint64_t SPSR_EL1H      = 0x05;  // EL1 using SP_EL1

} // namespace aarch64

#endif // STELLUX_ARCH_AARCH64_DEFS_EXCEPTION_H
