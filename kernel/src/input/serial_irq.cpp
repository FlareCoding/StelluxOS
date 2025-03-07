#include <input/serial_irq.h>
#include <input/system_input_manager.h>
#include <serial/serial.h>

namespace input {
__PRIVILEGED_CODE
irqreturn_t __com1_irq_handler(ptregs*, void*) {
    // System-wide input subsystem manager
    auto& input_manager = input::system_input_manager::get();

    // Read the input character from serial port
    char input_char = serial::read(SERIAL_PORT_BASE_COM1);
    
    // Create an input event structure
    input::input_event_t evt;
    zeromem(&evt, sizeof(input::input_event_t));
    evt.type = input::KBD_EVT_KEY_RELEASED;
    evt.sdata1 = static_cast<int32_t>(input_char);
    
    // Push the event into the kernel input queue
    input_manager.push_event(INPUT_QUEUE_ID_KBD, evt);

    return IRQ_HANDLED;
}
} // namespace input
