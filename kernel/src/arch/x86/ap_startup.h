#ifndef AP_STARTUP_H
#define AP_STARTUP_H
#include "apic.h"
#include "msr.h"

/**
 * @brief Boots and initializes all secondary Application Processor (AP) cores.
 *
 * This function handles the process of bringing up all secondary AP cores 
 * in the system. It sends the necessary startup signals to each AP core and 
 * initializes them to be part of the system's SMP (Symmetric Multiprocessing) 
 * environment, making them ready to execute tasks.
 */
void initializeApCores();

/**
 * @brief Boots and initializes a secondary Application Processor (AP) core.
 *
 * This function handles the process of bringing up an AP core by sending the
 * necessary startup signals and configuring its initial state. Once booted, the
 * core is initialized and ready to handle tasks as part of the system's SMP (Symmetric
 * Multiprocessing) environment.
 *
 * @param apicid The cpu ID of the AP core to be booted and initialized.
 */
void bootAndInitApCore(uint8_t apicid);

#endif
