#ifndef STELLUX_ARCH_X86_64_POWER_S5_H
#define STELLUX_ARCH_X86_64_POWER_S5_H

#include "common/types.h"

namespace power {

/**
 * @brief Extract the SLP_TYP values from the \_S5 package in AML bytecode.
 * Scans for the Name(_S5_, Package(...)) pattern instead of interpreting
 * the bytecode, which works because firmware defines \_S5 as a package of
 * integer constants.
 * @param aml    DSDT definition block, exclusive of the table header.
 * @param len    Length of the definition block in bytes.
 * @param typ_a  Receives the SLP_TYPa value for PM1a_CNT.
 * @param typ_b  Receives the SLP_TYPb value for PM1b_CNT.
 * @return true when both values were found and decoded.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool find_s5_sleep_types(const uint8_t* aml, size_t len,
                                           uint8_t* typ_a, uint8_t* typ_b);

} // namespace power

#endif // STELLUX_ARCH_X86_64_POWER_S5_H
