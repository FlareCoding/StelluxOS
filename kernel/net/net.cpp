#include "net/net.h"
#include "net/interface.h"
#include "sched/sched.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"

namespace net {

static void netstk_daemon_task_start(void*) {
    // All the network stack bookkeeping will be done here

    while (true) {
        RUN_ELEVATED(sched::sleep_ms(100));
    }

    sched::exit(0);
}

__PRIVILEGED_CODE int32_t init() {
    // Create the interface table
    // ...

    // Create and start the network stack daemon task
    sched::task* daemon = sched::create_kernel_task(
        netstk_daemon_task_start, nullptr, "netstkd");
    if (!daemon) {
        log::error("net: failed to create network stack daemon");
        return ERR_NO_MEMORY;
    }

    // Schedule the network stack daemon task
    sched::enqueue(daemon);

    return OK;
}

} // namespace net
