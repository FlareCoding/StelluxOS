#include <serial/serial.h>
#include <sched/sched.h>
#include <time/time.h>
#include <dynpriv/dynpriv.h>
#include <process/elf/elf64_loader.h>

int main() {
    RUN_ELEVATED({
        // Start a shell process
        task_control_block* task = elf::elf64_loader::load_from_file("/initrd/bin/shell");
        if (!task) {
            return -1;
        }

        // Allow the process to elevate privileges
        dynpriv::whitelist_asid(task->mm_ctx.root_page_table);
        sched::scheduler::get().add_task(task);
    });

    return 0;
}