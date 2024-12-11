#ifdef ARCH_X86_64
#ifndef PERCPU_OFFSETS_H
#define PERCPU_OFFSETS_H
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uintptr_t per_cpu_offset_current_task;
extern uintptr_t per_cpu_offset_default_kernel_stack;
extern uintptr_t per_cpu_offset_current_kernel_stack;
extern uintptr_t per_cpu_offset_current_user_stack;

#ifdef __cplusplus
}
#endif

#endif // PERCPU_OFFSETS_H
#endif // ARCH_X86_64
