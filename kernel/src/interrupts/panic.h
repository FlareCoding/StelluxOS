#ifndef PANIC_H
#define PANIC_H
#include "interrupts.h"

void kpanic(InterruptFrame* frame);

#endif
