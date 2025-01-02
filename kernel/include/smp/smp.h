#ifndef SMP_H
#define SMP_H
#include <types.h>

namespace smp {
/**
 * @brief Initializes Symmetric Multiprocessing (SMP) support.
 * 
 * Prepares the system to utilize multiple CPUs in a symmetric multiprocessing environment.
 * This function performs the necessary setup, including initializing per-CPU structures,
 * enabling inter-processor communication, and bootstrapping secondary CPUs.
 * 
 * @note This function must be called during system initialization to enable multi-core operation.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void smp_init();
} // namespace smp

#endif // SMP_H
