#ifndef SERIAL_IRQ_H
#define SERIAL_IRQ_H
#include <interrupts/irq.h>

namespace input {
/**
 * @brief Primary IRQ handler for COM1 serial input. Gets setup by the kernel at init stage.
 */
__PRIVILEGED_CODE irqreturn_t __com1_irq_handler(ptregs*, void*);
} // namespace input

#endif // SERIAL_IRQ_H

