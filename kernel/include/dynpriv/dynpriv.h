#ifndef DYNPRIV_H
#define DYNPRIV_H
#include <types.h>

namespace dynpriv {
/**
 * @brief Sets the current Address Space Identifier (ASID) for elevation checks.
 * 
 * This function configures the current ASID to be the one against which elevation
 * attempts are validated. By setting the current ASID, it ensures that only threads
 * within the kernel's blessed address space are permitted to elevate their hardware
 * privilege levels. This is crucial for maintaining the integrity and security of
 * the kernel's authoritative domain.
 */
__PRIVILEGED_CODE
void use_current_asid();

/**
 * @brief Checks if the current Address Space Identifier (ASID) is permitted to elevate.
 * 
 * This function verifies whether the current ASID is marked as "blessed" and thus
 * allowed to perform privilege elevation. It ensures that only authorized address spaces,
 * typically those belonging to kernel threads, can transition to higher hardware privilege
 * levels. This check is essential for preventing unauthorized privilege escalations.
 * 
 * @return true If the current ASID is allowed to elevate.
 * @return false If the current ASID is not permitted to elevate.
 */
__PRIVILEGED_CODE
bool is_asid_allowed();

/**
 * @brief Elevates the current thread's hardware privilege level.
 * 
 * This function transitions the executing thread from a lower to a higher hardware
 * privilege level within the kernel's blessed address space. Elevation grants the thread
 * access to privileged instructions and resources, enabling it to perform sensitive
 * operations that are restricted to higher privilege levels. It is essential that the
 * thread's ASID has been verified as allowed to elevate before invoking this function.
 */
void elevate();

/**
 * @brief Lowers the current thread's hardware privilege level.
 * 
 * This function transitions the executing thread from a higher to a lower hardware
 * privilege level, reducing its access to privileged instructions and resources. Lowering
 * is typically performed after completing operations that required elevated privileges to
 * ensure the thread operates within its designated privilege constraints.
 */
void lower();

/**
 * @brief Lowers the current thread's hardware privilege level and executes a target function.
 * 
 * This overloaded version of the `lower` function not only transitions the thread to a
 * lower hardware privilege level but also immediately invokes the specified target function
 * within the lowered privilege context. This is useful for performing specific operations
 * that should not retain elevated privileges beyond their immediate necessity.
 * 
 * @param target_fn A pointer to the function to be executed after lowering privileges.
 */
void lower(void* target_fn);

/**
 * @brief Checks if the current thread is operating at an elevated hardware privilege level.
 * 
 * This function determines whether the executing thread currently holds an elevated
 * hardware privilege level. It is useful for conditional operations that should only
 * occur when the thread has the necessary privileges. Understanding the current privilege
 * state helps maintain proper security and operational boundaries within the kernel.
 * 
 * @return true If the thread is operating at an elevated privilege level.
 * @return false If the thread is operating at a standard or lower privilege level.
 */
bool is_elevated();
} // namespace dynpriv

#endif // DYNPRIV_H

