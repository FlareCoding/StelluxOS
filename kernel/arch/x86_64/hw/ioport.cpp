#include "hw/ioport.h"
#include "hw/portio.h"

namespace ioport {

bool available() {
    return true;
}

__PRIVILEGED_CODE uint8_t read8(uint16_t port) {
    return portio::in8(port);
}

__PRIVILEGED_CODE void write8(uint16_t port, uint8_t value) {
    portio::out8(port, value);
}

} // namespace ioport
