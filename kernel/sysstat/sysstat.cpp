#include "sysstat/sysstat.h"
#include "fs/node.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "smp/smp.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"

namespace sysstat {

using devfs::append_str;
using devfs::append_u64;

static const char* task_state_name(uint32_t state) {
    switch (state) {
    case sched::TASK_STATE_CREATED: return "created";
    case sched::TASK_STATE_READY:   return "ready";
    case sched::TASK_STATE_RUNNING: return "running";
    case sched::TASK_STATE_BLOCKED: return "blocked";
    case sched::TASK_STATE_DEAD:    return "dead";
    default:                        return "unknown";
    }
}

static size_t generate_cpu(char* buf, size_t cap) {
    size_t pos = 0;

    uint32_t cpu_count = smp::cpu_count();
    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        sched::cpu_accounting_stats stats =
            sched::read_cpu_accounting_stats(cpu);

        pos = append_str(buf, cap, pos, "cpu");
        pos = append_u64(buf, cap, pos, cpu);
        pos = append_str(buf, cap, pos, " ");

        pos = append_u64(buf, cap, pos, stats.busy_ns);
        pos = append_str(buf, cap, pos, " ");
        pos = append_u64(buf, cap, pos, stats.idle_ns);
        pos = append_str(buf, cap, pos, "\n");
    }

    return pos;
}

static size_t generate_mem(char* buf, size_t cap) {
    uint64_t total = pmm::total_page_count();
    uint64_t free_count = pmm::free_page_count();
    uint64_t used = total > free_count ? total - free_count : 0;

    size_t pos = 0;
    pos = append_str(buf, cap, pos, "page_size ");
    pos = append_u64(buf, cap, pos, pmm::PAGE_SIZE);
    pos = append_str(buf, cap, pos, "\ntotal_pages ");
    pos = append_u64(buf, cap, pos, total);

    pos = append_str(buf, cap, pos, "\nfree_pages ");
    pos = append_u64(buf, cap, pos, free_count);
    pos = append_str(buf, cap, pos, "\nused_pages ");
    pos = append_u64(buf, cap, pos, used);
    pos = append_str(buf, cap, pos, "\n");

    return pos;
}

static size_t generate_uptime(char* buf, size_t cap) {
    size_t pos = append_u64(buf, cap, 0, clock::now_ns());
    return append_str(buf, cap, pos, "\n");
}

static size_t generate_tasks(char* buf, size_t cap) {
    size_t pos = 0;

    sync::irq_state irq = sched::g_task_registry.lock();
    sched::g_task_registry.for_each_locked([&](sched::task& t) {
        pos = append_u64(buf, cap, pos, t.tid);
        pos = append_str(buf, cap, pos, " ");
        pos = append_u64(buf, cap, pos, t.group ? t.group->pid : 0);
        pos = append_str(buf, cap, pos, " ");
        pos = append_str(buf, cap, pos, task_state_name(t.state.load_relaxed()));
        pos = append_str(buf, cap, pos, " ");

        pos = append_u64(buf, cap, pos, t.exec.cpu);
        pos = append_str(buf, cap, pos, " ");
        pos = append_u64(buf, cap, pos, sched::read_task_cpu_time_ns(&t));
        pos = append_str(buf, cap, pos, " ");
        pos = append_str(buf, cap, pos, t.name);
        pos = append_str(buf, cap, pos, "\n");
    });
    sched::g_task_registry.unlock(irq);

    return pos;
}

__PRIVILEGED_CODE int32_t init() {
    fs::node* dir = devfs::ensure_dir("sysinfo");
    if (!dir) {
        log::error("sysstat: failed to create /dev/sysinfo");
        return ERR;
    }

    struct {
        const char*                          name;
        devfs::text_snapshot_node::generator gen;
        size_t                               cap;
    } nodes[] = {
        { "cpu",    generate_cpu,    64 * MAX_CPUS },
        { "mem",    generate_mem,    128 },
        { "uptime", generate_uptime, 32 },
        { "tasks",  generate_tasks,  16384 },
    };

    for (auto& n : nodes) {
        void* mem = heap::kzalloc(sizeof(devfs::text_snapshot_node));
        if (!mem) {
            log::error("sysstat: failed to allocate /dev/sysinfo/%s", n.name);
            return ERR;
        }

        auto* node = new (mem) devfs::text_snapshot_node(n.name, n.gen, n.cap);

        if (devfs::add_char_device_at(dir, node) != devfs::OK) {
            log::error("sysstat: failed to register /dev/sysinfo/%s", n.name);
            node->~text_snapshot_node();
            heap::kfree(mem);
            return ERR;
        }
    }

    return OK;
}

} // namespace sysstat
