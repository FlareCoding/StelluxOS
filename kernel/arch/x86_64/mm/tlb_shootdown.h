#ifndef STELLUX_ARCH_X86_64_MM_TLB_SHOOTDOWN_H
#define STELLUX_ARCH_X86_64_MM_TLB_SHOOTDOWN_H

#include "common/types.h"

namespace x86 {

/**
 * @brief Register the interrupt message that carries TLB flush requests to
 * other CPUs. Called once from `paging::init`, before any CPU can request a
 * system-wide flush.
 * @return paging::OK on success, paging::ERR_NO_RESOURCE when no message
 *         slot is left.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_tlb_shootdown();

} // namespace x86

#endif // STELLUX_ARCH_X86_64_MM_TLB_SHOOTDOWN_H
