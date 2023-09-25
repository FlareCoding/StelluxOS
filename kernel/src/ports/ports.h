#ifndef PORTS_H
#define PORTS_H
#include <ktypes.h>

void outByte(uint16_t port, uint8_t value);

uint8_t inByte(uint16_t port);

#endif
