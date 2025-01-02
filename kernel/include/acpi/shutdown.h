#ifndef SHUTDOWN_H
#define SHUTDOWN_H
#include <types.h>

/**
 * @brief Shutdown the system when hosted within a Virtual Machine (VM).
 *
 * This function triggers a system shutdown specifically when the OS is running
 * inside a Virtual Machine environment. It ensures the VM performs a clean 
 * shutdown by invoking appropriate ACPI (Advanced Configuration and Power 
 * Interface) or virtual machine-specific mechanisms.
 *
 * The behavior of this function may vary depending on the hypervisor or VM 
 * technology (e.g., QEMU, VMware, VirtualBox) hosting the OS. It is designed 
 * to safely stop all processes and power down the system in a controlled manner.
 *
 * This is commonly used when testing the OS within a virtualized environment 
 * and can be hooked into scenarios where a manual or automatic shutdown is required.
 *
 * @note This function assumes the OS can detect that it is running inside a VM.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void vmshutdown();

#endif