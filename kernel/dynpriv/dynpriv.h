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

#endif // STELLUX_DYNPRIV_DYNPRIV_H
