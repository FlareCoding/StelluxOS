#include "debug/debug.h"
#include "debug/symtab.h"
#include "common/logging.h"

namespace debug {

__PRIVILEGED_CODE int32_t init() {
    int32_t rc = symtab::init();
    if (rc != symtab::OK) {
        log::warn("debug: symbol table unavailable (error %d), "
                  "crash dumps will show raw addresses", rc);
    }
    return OK;
}

} // namespace debug
