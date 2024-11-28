#ifndef PORTS_H
#define PORTS_H
#include <types.h>

void outb(uint16_t port, uint8_t value);
uint8_t inb(uint16_t port);

void outw(uint16_t port, uint16_t val);
uint16_t inw(uint16_t port);

void outl(uint16_t port, uint32_t val);
uint32_t inl(uint16_t port);

#endif
