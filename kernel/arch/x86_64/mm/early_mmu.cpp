#include "mm/early_mmu.h"

namespace early_mmu {

// x86_64 uses port I/O for early serial, no MMU setup needed.
// This no-op implementation satisfies the common interface.

__PRIVILEGED_CODE int32_t init() {
    return OK;
}

__PRIVILEGED_CODE uintptr_t map_device(uintptr_t phys_addr, [[maybe_unused]] size_t size) {
    return phys_addr;
}

} // namespace early_mmu
