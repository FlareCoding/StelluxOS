#ifdef ARCH_X86_64
#include <arch/percpu.h>
#include <process/process.h>

uintptr_t per_cpu_offset_current_task = PER_CPU_OFFSET(current_task);
uintptr_t per_cpu_offset_default_kernel_stack = PER_CPU_OFFSET(default_kernel_stack);
uintptr_t per_cpu_offset_current_kernel_stack = PER_CPU_OFFSET(current_kernel_stack);
uintptr_t per_cpu_offset_current_user_stack = PER_CPU_OFFSET(current_user_stack);

#endif // ARCH_X86_64

