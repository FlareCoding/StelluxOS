#ifndef KPRINT_H
#define KPRINT_H
#include <ktypes.h>

// Debug output goes to the debug serial port
void dbgPrint(const char* fmt, ...);

void kprintf(const char* fmt, ...);

#endif
