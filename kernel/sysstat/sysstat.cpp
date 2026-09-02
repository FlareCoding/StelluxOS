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

// Bounded text append helpers. Output past cap is dropped silently, so
// an overfull snapshot ends with a truncated final line.

static size_t append_str(char* buf, size_t cap, size_t pos, const char* s) {
    while (*s && pos < cap) {
        buf[pos++] = *s++;
    }
    return pos;
}

static size_t append_u64(char* buf, size_t cap, size_t pos, uint64_t value) {
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value > 0);

    while (count > 0 && pos < cap) {
        buf[pos++] = digits[--count];
    }
    return pos;
}

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
    pos = append_str(buf, cap, pos, "tick_hz ");
    pos = append_u64(buf, cap, pos,
                     sched::read_cpu_accounting_stats(0).tick_hz);
    pos = append_str(buf, cap, pos, "\n");

    uint32_t cpu_count = smp::cpu_count();
    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        sched::cpu_accounting_stats stats =
            sched::read_cpu_accounting_stats(cpu);

        pos = append_str(buf, cap, pos, "cpu");
        pos = append_u64(buf, cap, pos, cpu);
        pos = append_str(buf, cap, pos, " ");

        pos = append_u64(buf, cap, pos, stats.busy_ticks);
        pos = append_str(buf, cap, pos, " ");
        pos = append_u64(buf, cap, pos, stats.idle_ticks);
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
        pos = append_u64(buf, cap, pos, t.run_ticks.load_relaxed());
        pos = append_str(buf, cap, pos, " ");
        pos = append_str(buf, cap, pos, t.name);
        pos = append_str(buf, cap, pos, "\n");
    });
    sched::g_task_registry.unlock(irq);

    return pos;
}

namespace {

/**
 * A readable devfs text node. Every open holds a private snapshot buffer,
 * a read from offset zero regenerates it and later reads serve the same
 * bytes, so each reader sees one consistent capture.
 */
class stats_node : public fs::node {
public:
    using generator = size_t (*)(char* buf, size_t cap);

    stats_node(const char* name, generator gen, size_t cap)
        : fs::node(fs::node_type::char_device, nullptr, name),
          m_generate(gen), m_cap(cap) {}

    int32_t open(fs::file* f, uint32_t) override {
        // Snapshots hold formatted text on its way to userland, so
        // they live in unprivileged memory like the file that owns them
        void* mem = heap::uzalloc(sizeof(snapshot) + m_cap);
        if (!mem) {
            return fs::ERR_NOMEM;
        }

        auto* snap = static_cast<snapshot*>(mem);
        snap->text = reinterpret_cast<char*>(mem) + sizeof(snapshot);
        snap->len = 0;

        f->set_private_data(snap);
        return fs::OK;
    }

    int32_t on_close(fs::file* f) override {
        auto* snap = static_cast<snapshot*>(f->private_data());
        if (snap) {
            f->set_private_data(nullptr);
            heap::ufree(snap);
        }

        return fs::OK;
    }

    ssize_t read(fs::file* f, void* buf, size_t count) override {
        if (!f || !buf) {
            return fs::ERR_BADF;
        }

        auto* snap = static_cast<snapshot*>(f->private_data());
        if (!snap) {
            return fs::ERR_BADF;
        }

        int64_t off = f->offset();
        if (off < 0) {
            return fs::ERR_INVAL;
        }

        if (off == 0) {
            snap->len = m_generate(snap->text, m_cap);
        }

        size_t offset = static_cast<size_t>(off);
        if (offset >= snap->len) {
            return 0;
        }

        if (offset + count > snap->len) {
            count = snap->len - offset;
        }

        string::memcpy(buf, snap->text + offset, count);
        f->set_offset(static_cast<int64_t>(offset + count));
        return static_cast<ssize_t>(count);
    }

private:
    struct snapshot {
        char*  text;
        size_t len;
    };

    generator m_generate;
    size_t    m_cap;
};

} // anonymous namespace

__PRIVILEGED_CODE int32_t init() {
    fs::node* dir = devfs::ensure_dir("sysinfo");
    if (!dir) {
        log::error("sysstat: failed to create /dev/sysinfo");
        return ERR;
    }

    struct {
        const char*           name;
        stats_node::generator gen;
        size_t                cap;
    } nodes[] = {
        { "cpu",    generate_cpu,    64 * MAX_CPUS },
        { "mem",    generate_mem,    128 },
        { "uptime", generate_uptime, 32 },
        { "tasks",  generate_tasks,  16384 },
    };

    for (auto& n : nodes) {
        void* mem = heap::kzalloc(sizeof(stats_node));
        if (!mem) {
            log::error("sysstat: failed to allocate /dev/sysinfo/%s", n.name);
            return ERR;
        }

        auto* node = new (mem) stats_node(n.name, n.gen, n.cap);

        if (devfs::add_char_device_at(dir, node) != devfs::OK) {
            log::error("sysstat: failed to register /dev/sysinfo/%s", n.name);
            node->~stats_node();
            heap::kfree(mem);
            return ERR;
        }
    }

    return OK;
}

} // namespace sysstat
