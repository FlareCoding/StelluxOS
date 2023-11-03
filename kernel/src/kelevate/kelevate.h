#ifndef KELEVATE_H
#define KELEVATE_H
#include <ktypes.h>

using lowered_entry_fn_t = void(*)();

void __kelevate();
void __klower();

void __call_lowered_entry(lowered_entry_fn_t entry, void* user_stack);

#define RUN_ELEVATED2(code)      \
                __kelevate();   \
                code            \
                __klower();

#define RUN_ELEVATED(code) code

#endif