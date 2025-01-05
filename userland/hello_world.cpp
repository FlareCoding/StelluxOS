#include <types.h>
#include <serial/serial.h>
#include <sched/sched.h>
#include <time/time.h>

int main() {
    serial::printf("Userland code executed!\n");
    serial::printf("sleep(1) called\n");
    sleep(1);
    
    serial::printf("Exiting userland process... (well, spinning for now...)\n");
    while (1);
    return 0;
}
