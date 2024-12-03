#include <process/process.h>

DEFINE_PER_CPU(task_control_block*, current_task);
DEFINE_PER_CPU(uintptr_t, default_kernel_stack);
DEFINE_PER_CPU(uintptr_t, current_kernel_stack);
DEFINE_PER_CPU(uintptr_t, current_user_stack);

