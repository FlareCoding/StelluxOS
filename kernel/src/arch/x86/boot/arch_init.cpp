#include <types.h>
#include <serial/serial.h>

namespace arch {
    void arch_init() {
        serial::com1_printf("Hello :D\n");
    }
}
