#ifndef PORTS_H
#define PORTS_H
#include <ktypes.h>

__PRIVILEGED_CODE
void outByte(uint16_t port, uint8_t value);

__PRIVILEGED_CODE
uint8_t inByte(uint16_t port);

#endif
