#ifndef STELLUX_DYNPRIV_DYNPRIV_H
#define STELLUX_DYNPRIV_DYNPRIV_H

#include "common/types.h"

namespace dynpriv {

// Transition from Ring 3 / EL0 to Ring 0 / EL1
void elevate();

// Transition from Ring 0 / EL1 to Ring 3 / EL0
void lower();

// Check if currently elevated (Ring 0 / EL1)
bool is_elevated();

} // namespace dynpriv

#define RUN_ELEVATED(code)                          \
    do {                                            \
        bool was_elevated = dynpriv::is_elevated(); \
        if (!was_elevated) {                        \
            dynpriv::elevate();                     \
        }                                           \
        code;                                       \
        if (!was_elevated) {                        \
            dynpriv::lower();                       \
        }                                           \
    } while (0)

#endif // STELLUX_DYNPRIV_DYNPRIV_H
