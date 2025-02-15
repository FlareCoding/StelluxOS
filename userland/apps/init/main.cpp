#include <serial/serial.h>
#include <sched/sched.h>
#include <time/time.h>
#include <dynpriv/dynpriv.h>
#include <process/elf/elf64_loader.h>

bool start_process(const kstl::string& name) {
    RUN_ELEVATED({
        // Start a shell process
        task_control_block* task = elf::elf64_loader::load_from_file(name.c_str());
        if (!task) {
            serial::printf("[INIT] Failed to start '%s'!\n", name.c_str());
            return false;
        }

        // Allow the process to elevate privileges
        dynpriv::whitelist_asid(task->mm_ctx.root_page_table);
        sched::scheduler::get().add_task(task);
    });

    return true;
}

int main() {
    if (!start_process("/initrd/bin/shell")) {
        return -1;
    }

    if (!start_process("/initrd/bin/gfx_manager")) {
        return -1;
    }

    // User applications
    sleep(4);

    if (!start_process("/initrd/bin/pong")) {
        return -1;
    }

    return 0;
}