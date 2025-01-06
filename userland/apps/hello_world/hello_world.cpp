#include <types.h>
#include <serial/serial.h>
#include <sched/sched.h>
#include <time/time.h>
#include <dynpriv/dynpriv.h>

int main() {
    serial::printf("Userland code executed!\n");
    serial::printf("sleep(1) called\n");
    sleep(1);
    
    serial::printf("Exiting userland process in 100ms...\n");
    msleep(100);
    sched::exit_thread();
    return 0;
}
