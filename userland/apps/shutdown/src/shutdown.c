#include <stdio.h>
#include <sys/reboot.h>

int main(void) {
    printf("Powering off...\n");
    fflush(stdout);

    reboot(RB_POWER_OFF);

    // Only reachable when the platform could not power off, so stop
    // the machine in a state that is safe to switch off by hand.
    printf("Power off unsupported, halting instead.\n");
    fflush(stdout);
    reboot(RB_HALT_SYSTEM);

    perror("shutdown");
    return 1;
}
