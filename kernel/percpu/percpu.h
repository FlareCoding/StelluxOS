#ifndef STELLUX_PERCPU_PERCPU_H
#define STELLUX_PERCPU_PERCPU_H

#include "common/types.h"

extern "C" char __percpu_start[];
extern "C" char __percpu_end[];

extern uintptr_t __per_cpu_offset[MAX_CPUS];

#define DEFINE_PER_CPU_BASE(type, name) \
    __attribute__((section(".bss.percpu..base"))) \
    __typeof__(type) name
#define DECLARE_PER_CPU_BASE(type, name) \
    extern __attribute__((section(".bss.percpu..base"))) \
    __typeof__(type) name

#define DEFINE_PER_CPU(type, name) \
    __attribute__((section(".bss.percpu"))) \
    __typeof__(type) name
#define DECLARE_PER_CPU(type, name) \
    extern __attribute__((section(".bss.percpu"))) \
    __typeof__(type) name

#define DEFINE_PER_CPU_CACHELINE_ALIGNED(type, name) \
    __attribute__((section(".bss.percpu..cacheline_aligned"), aligned(64))) \
    __typeof__(type) name
#define DECLARE_PER_CPU_CACHELINE_ALIGNED(type, name) \
    extern __attribute__((section(".bss.percpu..cacheline_aligned"), aligned(64))) \
    __typeof__(type) name

DECLARE_PER_CPU_BASE(uintptr_t, percpu_offset);

namespace percpu {

inline uintptr_t size() {
    return reinterpret_cast<uintptr_t>(__percpu_end) -
           reinterpret_cast<uintptr_t>(__percpu_start);
}

uintptr_t this_cpu_offset();

template<typename T>
inline T* this_cpu_ptr(T& var) {
    static_assert(__is_trivially_copyable(T),
                  "Per-CPU variables must be trivially copyable");

    const uintptr_t off = this_cpu_offset();
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(&var) + off);
}

constexpr int32_t OK = 0;
constexpr int32_t ERR_SIZE_MISMATCH = -1;
constexpr int32_t ERR_LAYOUT = -2;

/**
 * Initialize per-CPU data for the BSP.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_bsp();

} // namespace percpu

#define this_cpu(var) (*percpu::this_cpu_ptr(var))

#endif // STELLUX_PERCPU_PERCPU_H
