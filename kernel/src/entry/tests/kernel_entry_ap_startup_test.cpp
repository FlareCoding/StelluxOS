#include "kernel_entry_tests.h"
#include <arch/x86/ap_startup.h>
#include <kprint.h>
#include <sched/sched.h>
#include <time/ktime.h>

void sayHelloCore() {
    kuPrint("Hello from core %i!\n", getCurrentCpuId());
    exitKernelThread();
}

void sayGoodbyeCore() {
    kuPrint("Goodbye from core %i!\n", getCurrentCpuId());
    exitKernelThread();
}

void ke_test_ap_startup() {
    auto& sched = RRScheduler::get();

    for (int i = 0; i < 10; i++) {
        Task* helloTask = createKernelTask(sayHelloCore);
        Task* goodbyeTask = createKernelTask(sayGoodbyeCore);

        sched.addTask(helloTask);
        sched.addTask(goodbyeTask);
    }
}
