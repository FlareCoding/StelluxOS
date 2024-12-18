#include <acpi/shutdown.h>
#include <ports/ports.h>

__PRIVILEGED_CODE
void vmshutdown() {
    const char s[] = "Shutdown";
    const char *p;

    /* 
        ACPI Shutdown sequence supported by Bochs and QEMU
        https://wiki.osdev.org/Shutdown
        http://forum.osdev.org/viewtopic.php?t=16990
    */
    outw(0x604, 0x0 | 0x2000);

    /*
        This is a special power-off sequence supported by Bochs and
        QEMU, but not by physical hardware.
    */
    for (p = s; *p != '\0'; p++) {
        outb(0x8900, *p);
    }

    /*
        This will power off a VMware VM if "gui.exitOnCLIHLT = TRUE"
        is set in its configuration file.
    */
    asm volatile ("cli");
    asm volatile ("hlt" : : : "memory");

    for (;;);
}
