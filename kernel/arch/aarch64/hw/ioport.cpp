#include "hw/ioport.h"

namespace ioport {

// What an I/O port with no device behind it reads on a PC
constexpr uint8_t NO_DEVICE = 0xFF;

// Stellux reaches PCI I/O space only through x86_64's port instructions
bool available() {
    return false;
}

__PRIVILEGED_CODE uint8_t read8(uint16_t) {
    return NO_DEVICE;
}

__PRIVILEGED_CODE void write8(uint16_t, uint8_t) {
}

} // namespace ioport
