#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/va_layout.h"
#include "mm/kva.h"
#include "mm/vmm.h"
#include "common/utils/logging.h"

namespace mm {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    if (pmm::init() != pmm::OK) {
        log::error("mm: pmm init failed");
        return ERR;
    }

    if (init_va_layout() != VA_LAYOUT_OK) {
        log::error("mm: va_layout init failed");
        return ERR;
    }

    if (kva::init() != kva::OK) {
        log::error("mm: kva init failed");
        return ERR;
    }

    if (vmm::init() != vmm::OK) {
        log::error("mm: vmm init failed");
        return ERR;
    }

    return OK;
}

} // namespace mm
