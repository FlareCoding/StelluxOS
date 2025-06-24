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
    
    // Create a keyboard key pressed event structure
    input::input_event_t evt{};
    evt.id = 0;
    evt.type = input::KBD_EVT_KEY_PRESSED;
    evt.udata1 = static_cast<uint32_t>(input_char);
    evt.udata2 = 0; // No modifiers for serial input
    evt.sdata1 = static_cast<int32_t>(input_char);
    evt.sdata2 = 0;
    
    // Push the event into the kernel input queue
    input_manager.push_event(INPUT_QUEUE_ID_SYSTEM, *reinterpret_cast<input::input_event_t*>(&evt));

    return IRQ_HANDLED;
}
} // namespace input
