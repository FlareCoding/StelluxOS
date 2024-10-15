#include "kprint.h"
#include "kstring.h"
#include <drivers/serial/serial_driver.h>
#include <drivers/graphics/vga_text_driver.h>
#include <process/process.h>
#include <arch/x86/per_cpu_data.h>
#include <kelevate/kelevate.h>
#include <sched/sched.h>

void dbgPrint(const char* fmt, ...) {
    char buf[512] = { 0 };

    va_list args;
    
    // Initialize the va_list with the variable arguments
    va_start(args, fmt);
    
    // Pass the va_list to the actual formatStringCopy function
    _vaformatStringCopy(buf, sizeof(buf) - 1, fmt, args);
    
    // Clean up the va_list
    va_end(args);

    SerialDriver::writePort(SERIAL_PORT_BASE_COM1, buf);
}

void kprintf(const char* fmt, ...) {
    char buf[512] = { 0 };

    va_list args;
    
    // Initialize the va_list with the variable arguments
    va_start(args, fmt);
    
    // Pass the va_list to the actual formatStringCopy function
    _vaformatStringCopy(buf, sizeof(buf) - 1, fmt, args);
    
    // Clean up the va_list
    va_end(args);

    SerialDriver::writePort(SERIAL_PORT_BASE_COM1, buf);

    Scheduler::get().preemptDisable();

    RUN_ELEVATED({
        VGATextDriver::renderString(buf);
        VGADriver::swapBuffers();
    });

    Scheduler::get().preemptEnable();
}

