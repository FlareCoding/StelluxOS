#include <stdio.h>
#include <sys/reboot.h>

int main(void) {
    printf("Restarting system...\n");
    fflush(stdout);

    reboot(RB_AUTOBOOT);

    // Only reachable when the kernel could not restart the machine
    perror("reboot");
    return 1;
}
