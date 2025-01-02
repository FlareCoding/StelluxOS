#ifndef PERCPU_H
#define PERCPU_H
#include <types.h>

#define MAX_SYSTEM_CPUS 64

#define BSP_CPU_ID 0

extern char __per_cpu_start;
extern char __per_cpu_end;
extern char __per_cpu_size;

#define DECLARE_PER_CPU(type, name) \
    extern type name

#define DEFINE_PER_CPU(type, name) \
    __attribute__((section(".percpu,\"\",@nobits#"), aligned(sizeof(type)))) type name

#define PER_CPU_OFFSET(name) ((uintptr_t)&name - (uintptr_t)&__per_cpu_start)

#ifdef ARCH_X86_64
/**
 * @brief Reads a per-CPU variable.
 * 
 * This template function retrieves the value of a per-CPU variable by accessing the
 * appropriate memory location using the GS segment register. It ensures that each
 * CPU core can access its own instance of the variable without interference from other cores.
 * 
 * @tparam T The type of the per-CPU variable.
 * @param name Reference to the per-CPU variable to be read.
 * @return T The value of the specified per-CPU variable.
 */
template <typename T>
inline T this_cpu_read(T& name) {
    T __x;
    uintptr_t __offset = PER_CPU_OFFSET(name);
    asm volatile("mov %%gs:(%1), %0"
                 : "=r"(__x)
                 : "r"(__offset));
    return __x;
}

/**
 * @brief Writes to a per-CPU variable.
 * 
 * This template function sets the value of a per-CPU variable by writing to the
 * appropriate memory location using the GS segment register. It ensures that each
 * CPU core can modify its own instance of the variable without affecting other cores.
 * 
 * @tparam T The type of the per-CPU variable.
 * @param name Reference to the per-CPU variable to be written to.
 * @param val The value to assign to the specified per-CPU variable.
 */
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
/**
 * @brief Initializes the per-CPU area for the Bootstrap Processor (BSP).
 * 
 * Sets up the memory area used to store per-CPU data for the BSP, ensuring proper initialization
 * of CPU-specific structures and data.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_bsp_per_cpu_area();

/**
 * @brief Initializes the per-CPU area for an Application Processor (AP).
 * @param cpu_id The ID of the AP to initialize the per-CPU area for.
 * 
 * Sets up the memory area for storing per-CPU data for the specified AP, ensuring that CPU-specific
 * data structures are properly initialized.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_ap_per_cpu_area(uint8_t cpu_id);

/**
 * @brief Allocates memory for the per-CPU area of an Application Processor (AP).
 * @param cpu_id The ID of the AP to allocate the per-CPU area for.
 * 
 * Dynamically allocates the required memory for the per-CPU area associated with the specified AP.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void allocate_ap_per_cpu_area(uint8_t cpu_id);

/**
 * @brief Deallocates the per-CPU area of an Application Processor (AP).
 * @param cpu_id The ID of the AP to deallocate the per-CPU area for.
 * 
 * Frees the memory allocated for the per-CPU area of the specified AP, ensuring proper cleanup.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void deallocate_ap_per_cpu_area(uint8_t cpu_id);
} // namespace arch

#endif // PERCPU_H
