#ifndef PERCPU_H
#define PERCPU_H
#include <types.h>

extern char __per_cpu_start;
extern char __per_cpu_end;
extern char __per_cpu_size;

#define DECLARE_PER_CPU(type, name) \
    extern type name

#define DEFINE_PER_CPU(type, name) \
    __attribute__((section(".percpu,\"\",@nobits#"), aligned(sizeof(type)))) type name

#define PER_CPU_OFFSET(name) ((uintptr_t)&name - (uintptr_t)&__per_cpu_start)

#ifdef ARCH_X86_64
template <typename T>
inline T this_cpu_read(T& name) {
    T __x;
    uintptr_t __offset = PER_CPU_OFFSET(name);
    asm volatile("mov %%gs:(%1), %0"
                 : "=r"(__x)
                 : "r"(__offset));
    return __x;
}

template <typename T>
inline void this_cpu_write(T& name, T val) {
    uintptr_t __offset = PER_CPU_OFFSET(name);
    asm volatile("mov %0, %%gs:(%1)"
                 :
                 : "r"(val), "r"(__offset));
}
#else
template <typename T>
inline T this_cpu_read(T& name) {}

template <typename T>
inline void this_cpu_write(T& name, T val) {}
#endif

namespace arch {
__PRIVILEGED_CODE
void init_bsp_per_cpu_area();
} // namespace arch

#endif // PERCPU_H
