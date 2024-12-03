#ifndef DYNPRIV_H
#define DYNPRIV_H
#include <types.h>

namespace dynpriv {
__PRIVILEGED_CODE
void use_current_asid();

__PRIVILEGED_CODE
bool is_asid_allowed();

void elevate();
void lower();
void lower(void* target_fn);

bool is_elevated();
} // namespace dynpriv

#endif // DYNPRIV_H

