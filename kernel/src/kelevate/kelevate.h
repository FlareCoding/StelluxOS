#ifndef KELEVATE_H
#define KELEVATE_H
#include <ktypes.h>
#include <arch/x86/per_cpu_data.h>

using lowered_entry_fn_t = void(*)();

void __kelevate();
void __klower();

void __call_lowered_entry(lowered_entry_fn_t entry, void* user_stack);

#define RUN_ELEVATED(code)                                      \
            do {                                                \
                bool initiallyElevated = current->elevated;     \
                if (!initiallyElevated) __kelevate();           \
                code                                            \
                if (!initiallyElevated) __klower();             \
            } while (0)

#endif