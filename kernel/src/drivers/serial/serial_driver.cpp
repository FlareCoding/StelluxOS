#include "serial_driver.h"
#include <kelevate/kelevate.h>
#include <interrupts/interrupts.h>
#include <arch/x86/apic.h>
#include <kprint.h>

#define COM1_IRQ_LINE 4
#define COM2_IRQ_LINE 3

irqreturn_t serialPortCom1IrqHandler(PtRegs*, void*) {
    char c = readFromSerialPort(SERIAL_PORT_BASE_COM1);

    // Character correction for serial emulators
    switch (c) {
    case '\r': {
        c = '\n';
        break;
    }
    case 127: {
        c = '\b';
        break;
    }
    default: break;
    }

    if (current->console && (current->console->checkSerialConnection() == SERIAL_PORT_BASE_COM1)) {
        if (c == '\b') {
            // Erase last character and move cursor back
            current->console->write("\b \b");
        } else {
            char buf[2] = { c, 0x00 };
            current->console->write(buf);
        }
    }

    Apic::getLocalApic()->completeIrq();
    return IRQ_HANDLED;
}

void SerialDriver::init() {
    routeIoApicIrq(COM1_IRQ_LINE, IRQ4);

    registerIrqHandler(IRQ4, serialPortCom1IrqHandler, false, nullptr);
}

void SerialDriver::writePort(uint16_t port, const char* buffer) {
    writeToSerialPort(port, buffer);
}

char SerialDriver::readPort(uint16_t port) {
    char c = (char)0x00;
    c = readFromSerialPort(port);

    return c;
}