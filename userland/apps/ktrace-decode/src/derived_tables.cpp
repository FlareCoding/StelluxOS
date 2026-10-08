#include "derived_tables.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

// A syscall or page fault of one thread. A record is written as the work ends, so its time
// is the end, and the start is the end minus the duration.
struct activity_span {
    uint64_t start_ns;
    uint64_t end_ns;
    uint32_t syscall_number;
};

struct thread_activity {
    std::vector<activity_span> syscalls;
    std::vector<activity_span> faults;
};

enum stretch_state : uint8_t {
    STRETCH_NONE,
    STRETCH_RUNNABLE,
    STRETCH_SLEEPING,
};

// A runnable or sleeping stretch that has started and not yet ended
struct open_stretch {
    stretch_state           state = STRETCH_NONE;
    uint64_t                start_ns = 0;
    std::optional<uint32_t> waker_tid;
    std::optional<uint8_t>  switch_reason;
    std::optional<uint32_t> syscall_number;
    bool                    in_page_fault = false;
};

struct thread_progress {
    bool                    has_appeared = false;
    uint64_t                first_ns = 0;
    uint64_t                last_ns = 0;
    uint8_t                 kind = ktrace::TASK_KIND_NOT_RECORDED;
    std::optional<uint32_t> switch_pid;
    std::optional<uint32_t> syscall_pid;
    std::optional<uint64_t> exit_ns;
    bool                    is_running = false;
    open_stretch            stretch;
    int64_t                 name_id = 0;
    uint64_t                name_first_ns = 0;
    uint64_t                name_last_ns = 0;
};

// What a CPU has run since its last switch
struct open_slice {
    bool     is_open = false;
    uint32_t tid = 0;
    uint8_t  kind = ktrace::TASK_KIND_NOT_RECORDED;
    uint64_t start_ns = 0;
};

struct process_progress {
    uint64_t threads = 0;
    uint64_t first_ns = UINT64_MAX;
    uint64_t last_ns = 0;
};

struct derived_writer {
    database&                                     db;
    const trace_file&                             file;
    name_ids&                                     ids;
    derived_counts&                               counts;
    std::string&                                  error;
    std::optional<uint64_t>                       stop_ns;
    std::unordered_map<uint32_t, thread_activity> activity_by_tid;
    std::unordered_map<uint32_t, thread_progress> thread_by_tid;
    std::vector<open_slice>                       slice_by_cpu;
    sqlite_statement                              insert_slice;
    sqlite_statement                              insert_state;
    sqlite_statement                              insert_thread_name;
};

constexpr const char* DERIVED_SCHEMA = R"sql(
CREATE TABLE cpu_slices (
    cpu               INTEGER NOT NULL,
    tid               INTEGER NOT NULL,  -- The idle task's tid while the CPU was idle
    start_ns          INTEGER NOT NULL,  -- The switch that put the thread on the CPU, or the session start
    end_ns            INTEGER            -- The CPU's next switch, NULL when unknown
);

CREATE TABLE thread_states (
    tid               INTEGER NOT NULL,
    state             TEXT NOT NULL,     -- running, runnable or sleeping
    start_ns          INTEGER NOT NULL,
    end_ns            INTEGER,           -- NULL when unknown
    cpu               INTEGER,           -- While running, the CPU it ran on
    waker_tid         INTEGER,           -- While runnable after a wakeup, the thread that woke it
    switch_reason     INTEGER,           -- While runnable after a switch-out, 0 preempted or 1 yielded
    syscall_number    INTEGER,           -- While sleeping, the syscall it slept in, NULL when no record shows one
    in_page_fault     INTEGER            -- While sleeping, 1 inside a page fault and 0 otherwise. Syscalls and faults
                                         -- are recorded as they end, so a sleep still running at the stop shows neither.
);

CREATE TABLE threads (
    tid               INTEGER PRIMARY KEY,
    kind              INTEGER,           -- 1 user, 2 kernel or 3 idle, NULL when no record carries it
    pid               INTEGER,           -- From its switch-ins, else its syscalls, NULL when no record has it
    first_ns          INTEGER NOT NULL,  -- When it first appears in any record
    last_ns           INTEGER NOT NULL,  -- When it last appears in any record
    exit_ns           INTEGER            -- The switch-out that ended it, NULL when it did not exit
);

CREATE TABLE thread_names (
    tid               INTEGER NOT NULL,
    name_id           INTEGER NOT NULL,  -- A names.name_id
    first_ns          INTEGER NOT NULL,  -- The first record showing the thread with this name
    last_ns           INTEGER NOT NULL   -- The last record showing the thread with this name
);

CREATE TABLE processes (
    pid               INTEGER PRIMARY KEY,
    leader_name_id    INTEGER,           -- The latest name of the thread whose tid is the pid
    threads           INTEGER NOT NULL,
    first_ns          INTEGER NOT NULL,
    last_ns           INTEGER NOT NULL
);
)sql";

constexpr const char* DERIVED_INDEXES = R"sql(
CREATE INDEX cpu_slices_by_cpu ON cpu_slices (cpu, start_ns);
CREATE INDEX cpu_slices_by_tid ON cpu_slices (tid, start_ns);
CREATE INDEX thread_states_by_tid ON thread_states (tid, start_ns);
CREATE INDEX thread_names_by_tid ON thread_names (tid, first_ns);
)sql";

constexpr const char* INSERT_SLICE_SQL = "INSERT INTO cpu_slices (cpu, tid, start_ns, end_ns) VALUES (?, ?, ?, ?)";

constexpr const char* INSERT_STATE_SQL =
    "INSERT INTO thread_states (tid, state, start_ns, end_ns, cpu, waker_tid, switch_reason, syscall_number, "
    "in_page_fault) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

constexpr const char* INSERT_THREAD_SQL =
    "INSERT INTO threads (tid, kind, pid, first_ns, last_ns, exit_ns) VALUES (?, ?, ?, ?, ?, ?)";

constexpr const char* INSERT_THREAD_NAME_SQL =
    "INSERT INTO thread_names (tid, name_id, first_ns, last_ns) VALUES (?, ?, ?, ?)";

constexpr const char* INSERT_PROCESS_SQL =
    "INSERT INTO processes (pid, leader_name_id, threads, first_ns, last_ns) VALUES (?, ?, ?, ?, ?)";

static void bind_optional_u64(sqlite3_stmt* row, int index, std::optional<uint64_t> value) {
    bind_u64_or_null(row, index, value.value_or(0), value.has_value());
}

// The span in progress at `ts_ns`, from one thread's spans in time order. A span whose
// estimated start falls after `ts_ns` never matches, so a cause is missed rather than wrong.
static const activity_span* span_at(const std::vector<activity_span>& spans, uint64_t ts_ns) {
    auto span = std::lower_bound(spans.begin(), spans.end(), ts_ns,
                                 [](const activity_span& candidate, uint64_t ts) { return candidate.end_ns < ts; });
    if (span == spans.end() || span->start_ns > ts_ns) {
        return nullptr;
    }

    return &*span;
}

static void collect_activity(derived_writer& w, const timeline& order) {
    for (const timeline_record& record : order.records) {
        const uint8_t* bytes = record_bytes(w.file, record);
        uint16_t event_id = record_event_id(bytes);
        if (event_id == ktrace::EVENT_SYSCALL) {
            auto payload = record_payload<ktrace::syscall_payload>(bytes);
            w.activity_by_tid[payload.tid].syscalls.push_back(
                {record.ts_ns - payload.duration_ns, record.ts_ns, payload.number});
        } else if (event_id == ktrace::EVENT_PAGE_FAULT) {
            auto payload = record_payload<ktrace::page_fault_payload>(bytes);
            w.activity_by_tid[payload.tid].faults.push_back({record.ts_ns - payload.duration_ns, record.ts_ns, 0});
        }
    }
}

static bool insert_slice_row(derived_writer& w, uint32_t cpu, const open_slice& slice, std::optional<uint64_t> end_ns) {
    sqlite3_stmt* row = w.insert_slice.get();
    sqlite3_bind_int64(row, 1, cpu);
    sqlite3_bind_int64(row, 2, slice.tid);
    bind_u64(row, 3, slice.start_ns);
    bind_optional_u64(row, 4, end_ns);
    w.counts.cpu_slices++;

    return insert_row(w.db, row, w.error);
}

static bool insert_running_row(derived_writer& w, uint32_t cpu, const open_slice& slice,
                               std::optional<uint64_t> end_ns) {
    sqlite3_stmt* row = w.insert_state.get();
    sqlite3_clear_bindings(row);
    sqlite3_bind_int64(row, 1, slice.tid);
    sqlite3_bind_text(row, 2, "running", -1, SQLITE_STATIC);
    bind_u64(row, 3, slice.start_ns);
    bind_optional_u64(row, 4, end_ns);
    sqlite3_bind_int64(row, 5, cpu);
    w.counts.thread_states++;

    return insert_row(w.db, row, w.error);
}

static bool insert_stretch_row(derived_writer& w, uint32_t tid, const open_stretch& stretch,
                               std::optional<uint64_t> end_ns) {
    sqlite3_stmt* row = w.insert_state.get();
    sqlite3_clear_bindings(row);
    sqlite3_bind_int64(row, 1, tid);
    sqlite3_bind_text(row, 2, stretch.state == STRETCH_RUNNABLE ? "runnable" : "sleeping", -1, SQLITE_STATIC);
    bind_u64(row, 3, stretch.start_ns);
    bind_optional_u64(row, 4, end_ns);

    if (stretch.state == STRETCH_RUNNABLE) {
        bind_optional_u64(row, 6, stretch.waker_tid);
        bind_optional_u64(row, 7, stretch.switch_reason);
    } else {
        bind_optional_u64(row, 8, stretch.syscall_number);
        sqlite3_bind_int(row, 9, stretch.in_page_fault ? 1 : 0);
    }

    w.counts.thread_states++;
    return insert_row(w.db, row, w.error);
}

// Ends what a CPU was running, as a slice and, for a thread that is not idle, a running state
static bool end_slice(derived_writer& w, uint32_t cpu, const open_slice& slice, std::optional<uint64_t> end_ns) {
    if (!insert_slice_row(w, cpu, slice, end_ns)) {
        return false;
    }

    if (slice.kind == ktrace::TASK_KIND_IDLE) {
        return true;
    }

    return insert_running_row(w, cpu, slice, end_ns);
}

static bool end_stretch(derived_writer& w, uint32_t tid, thread_progress& thread, std::optional<uint64_t> end_ns) {
    if (thread.stretch.state == STRETCH_NONE) {
        return true;
    }

    bool inserted = insert_stretch_row(w, tid, thread.stretch, end_ns);
    thread.stretch = {};

    return inserted;
}

static bool insert_thread_name_row(derived_writer& w, uint32_t tid, const thread_progress& thread) {
    sqlite3_stmt* row = w.insert_thread_name.get();
    sqlite3_bind_int64(row, 1, tid);
    sqlite3_bind_int64(row, 2, thread.name_id);
    bind_u64(row, 3, thread.name_first_ns);
    bind_u64(row, 4, thread.name_last_ns);

    return insert_row(w.db, row, w.error);
}

// Records that `tid` appears at `ts_ns` with the kind and name a switch or wakeup carried
static bool note_task(derived_writer& w, uint32_t tid, uint8_t kind, const char* name, uint64_t ts_ns,
                      thread_progress*& thread) {
    thread = &w.thread_by_tid[tid];
    if (!thread->has_appeared) {
        thread->has_appeared = true;
        thread->first_ns = ts_ns;
    }

    thread->last_ns = ts_ns;
    if (thread->kind == ktrace::TASK_KIND_NOT_RECORDED) {
        thread->kind = kind;
    }

    int64_t name_id = intern_task_name(w.ids, reinterpret_cast<const uint8_t*>(name));
    if (thread->name_id == name_id) {
        thread->name_last_ns = ts_ns;
        return true;
    }

    if (thread->name_id != 0 && !insert_thread_name_row(w, tid, *thread)) {
        return false;
    }

    thread->name_id = name_id;
    thread->name_first_ns = ts_ns;
    thread->name_last_ns = ts_ns;
    return true;
}

static thread_progress& note_thread(derived_writer& w, uint32_t tid, uint64_t ts_ns) {
    thread_progress& thread = w.thread_by_tid[tid];
    if (!thread.has_appeared) {
        thread.has_appeared = true;
        thread.first_ns = ts_ns;
    }

    thread.last_ns = ts_ns;
    return thread;
}

// The thread each CPU runs when the session starts appears only as the previous task of that
// CPU's first switch. It counts as running from the start, so a wakeup before then is ignored.
static void mark_session_start_runners(derived_writer& w, const timeline& order) {
    std::vector<bool> has_switched(w.slice_by_cpu.size(), false);
    size_t cpus_left = has_switched.size();
    for (const timeline_record& record : order.records) {
        if (cpus_left == 0) {
            break;
        }

        const uint8_t* bytes = record_bytes(w.file, record);
        if (record_event_id(bytes) != ktrace::EVENT_SCHED_SWITCH || has_switched[record.cpu]) {
            continue;
        }

        w.thread_by_tid[record_payload<ktrace::sched_switch_payload>(bytes).prev_tid].is_running = true;
        has_switched[record.cpu] = true;
        cpus_left--;
    }
}

static void start_stretch_after_switch_out(derived_writer& w, uint32_t tid, thread_progress& thread, uint8_t reason,
                                           uint64_t ts_ns) {
    thread.stretch = {};
    thread.stretch.start_ns = ts_ns;
    if (reason != ktrace::SWITCH_REASON_BLOCKED) {
        thread.stretch.state = STRETCH_RUNNABLE;
        thread.stretch.switch_reason = reason;
        return;
    }

    thread.stretch.state = STRETCH_SLEEPING;
    auto activity = w.activity_by_tid.find(tid);
    if (activity == w.activity_by_tid.end()) {
        return;
    }

    const activity_span* syscall = span_at(activity->second.syscalls, ts_ns);
    if (syscall) {
        thread.stretch.syscall_number = syscall->syscall_number;
    }

    thread.stretch.in_page_fault = span_at(activity->second.faults, ts_ns) != nullptr;
}

static bool on_switch(derived_writer& w, const timeline_record& record, const ktrace::sched_switch_payload& payload) {
    thread_progress* prev = nullptr;
    thread_progress* next = nullptr;
    if (!note_task(w, payload.prev_tid, payload.prev_kind, payload.prev_name, record.ts_ns, prev) ||
        !note_task(w, payload.next_tid, payload.next_kind, payload.next_name, record.ts_ns, next)) {
        return false;
    }

    // The stretch before a CPU's first switch belongs to that switch's previous task. Lost
    // records break a CPU's chain of switches, and then the open slice's end is unknown.
    open_slice& slice = w.slice_by_cpu[record.cpu];
    if (!slice.is_open) {
        open_slice first = {true, payload.prev_tid, payload.prev_kind, w.file.header.session_start_ns};
        if (!end_slice(w, record.cpu, first, record.ts_ns)) {
            return false;
        }
    } else {
        std::optional<uint64_t> end_ns = slice.tid == payload.prev_tid ? std::optional(record.ts_ns) : std::nullopt;
        if (!end_slice(w, record.cpu, slice, end_ns)) {
            return false;
        }
    }

    slice = {true, payload.next_tid, payload.next_kind, record.ts_ns};

    prev->is_running = false;
    if (!end_stretch(w, payload.prev_tid, *prev, std::nullopt)) {
        return false;
    }

    if (payload.reason == ktrace::SWITCH_REASON_EXITED) {
        prev->exit_ns = record.ts_ns;
    } else if (payload.prev_kind != ktrace::TASK_KIND_IDLE) {
        start_stretch_after_switch_out(w, payload.prev_tid, *prev, payload.reason, record.ts_ns);
    }

    if (!end_stretch(w, payload.next_tid, *next, record.ts_ns)) {
        return false;
    }

    next->is_running = true;
    if (payload.next_kind != ktrace::TASK_KIND_NOT_RECORDED) {
        next->switch_pid = payload.next_pid;
    }

    return true;
}

static bool on_wakeup(derived_writer& w, const timeline_record& record, const ktrace::sched_wakeup_payload& payload) {
    thread_progress* woken = nullptr;
    thread_progress* waker = nullptr;
    if (!note_task(w, payload.woken_tid, payload.woken_kind, payload.woken_name, record.ts_ns, woken) ||
        !note_task(w, payload.waker_tid, payload.waker_kind, payload.waker_name, record.ts_ns, waker)) {
        return false;
    }

    // A thread woken while still running was about to sleep, and its switch-out reads yielded
    if (woken->is_running || woken->exit_ns || woken->stretch.state == STRETCH_RUNNABLE ||
        payload.woken_kind == ktrace::TASK_KIND_IDLE) {
        return true;
    }

    if (!end_stretch(w, payload.woken_tid, *woken, record.ts_ns)) {
        return false;
    }

    woken->stretch = {};
    woken->stretch.state = STRETCH_RUNNABLE;
    woken->stretch.start_ns = record.ts_ns;
    woken->stretch.waker_tid = payload.waker_tid;
    return true;
}

static bool walk_timeline(derived_writer& w, const timeline& order) {
    for (const timeline_record& record : order.records) {
        const uint8_t* bytes = record_bytes(w.file, record);
        switch (record_event_id(bytes)) {
        case ktrace::EVENT_SCHED_SWITCH:
            if (!on_switch(w, record, record_payload<ktrace::sched_switch_payload>(bytes))) {
                return false;
            }

            break;
        case ktrace::EVENT_SCHED_WAKEUP:
            if (!on_wakeup(w, record, record_payload<ktrace::sched_wakeup_payload>(bytes))) {
                return false;
            }

            break;
        case ktrace::EVENT_SYSCALL: {
            auto payload = record_payload<ktrace::syscall_payload>(bytes);
            note_thread(w, payload.tid, record.ts_ns).syscall_pid = payload.pid;
            break;
        }
        case ktrace::EVENT_PAGE_FAULT:
            note_thread(w, record_payload<ktrace::page_fault_payload>(bytes).tid, record.ts_ns);
            break;
        }
    }

    return true;
}

// Ends what is still open when the records run out: at the session's stop, or unknown when cut short
static bool end_open_rows(derived_writer& w) {
    for (uint32_t cpu = 0; cpu < w.slice_by_cpu.size(); cpu++) {
        if (w.slice_by_cpu[cpu].is_open && !end_slice(w, cpu, w.slice_by_cpu[cpu], w.stop_ns)) {
            return false;
        }
    }

    for (auto& [tid, thread] : w.thread_by_tid) {
        if (!end_stretch(w, tid, thread, w.stop_ns)) {
            return false;
        }

        if (thread.name_id != 0 && !insert_thread_name_row(w, tid, thread)) {
            return false;
        }
    }

    return true;
}

static bool write_thread_rows(derived_writer& w) {
    sqlite_statement insert = prepare_statement(w.db, INSERT_THREAD_SQL, w.error);
    if (!insert) {
        return false;
    }

    std::map<uint32_t, const thread_progress*> threads_in_tid_order;
    for (const auto& [tid, thread] : w.thread_by_tid) {
        threads_in_tid_order[tid] = &thread;
    }

    sqlite3_stmt* row = insert.get();
    for (const auto& [tid, thread] : threads_in_tid_order) {
        std::optional<uint32_t> pid = thread->switch_pid ? thread->switch_pid : thread->syscall_pid;
        sqlite3_bind_int64(row, 1, tid);
        bind_optional_u64(row, 2, thread->kind == ktrace::TASK_KIND_NOT_RECORDED ? std::nullopt
                                                                                  : std::optional<uint64_t>(thread->kind));
        bind_optional_u64(row, 3, pid);
        bind_u64(row, 4, thread->first_ns);
        bind_u64(row, 5, thread->last_ns);
        bind_optional_u64(row, 6, thread->exit_ns);
        if (!insert_row(w.db, row, w.error)) {
            return false;
        }

        w.counts.threads++;
    }

    return true;
}

static bool write_process_rows(derived_writer& w) {
    std::map<uint32_t, process_progress> process_by_pid;
    for (const auto& [tid, thread] : w.thread_by_tid) {
        std::optional<uint32_t> pid = thread.switch_pid ? thread.switch_pid : thread.syscall_pid;
        if (!pid || *pid == 0) {
            continue;
        }

        process_progress& process = process_by_pid[*pid];
        process.threads++;
        process.first_ns = std::min(process.first_ns, thread.first_ns);
        process.last_ns = std::max(process.last_ns, thread.last_ns);
    }

    sqlite_statement insert = prepare_statement(w.db, INSERT_PROCESS_SQL, w.error);
    if (!insert) {
        return false;
    }

    sqlite3_stmt* row = insert.get();
    for (const auto& [pid, process] : process_by_pid) {
        auto leader = w.thread_by_tid.find(pid);
        bool has_leader_name = leader != w.thread_by_tid.end() && leader->second.name_id != 0;
        sqlite3_bind_int64(row, 1, pid);
        bind_u64_or_null(row, 2, has_leader_name ? leader->second.name_id : 0, has_leader_name);
        bind_u64(row, 3, process.threads);
        bind_u64(row, 4, process.first_ns);
        bind_u64(row, 5, process.last_ns);
        if (!insert_row(w.db, row, w.error)) {
            return false;
        }

        w.counts.processes++;
    }

    return true;
}

bool write_derived_tables(database& db, const trace_file& file, const timeline& order, name_ids& ids,
                          derived_counts& counts, std::string& error) {
    if (!execute_sql(db, DERIVED_SCHEMA, error)) {
        return false;
    }

    derived_writer w = {db, file, ids, counts, error, std::nullopt, {}, {}, {}, {nullptr, sqlite3_finalize},
                        {nullptr, sqlite3_finalize}, {nullptr, sqlite3_finalize}};
    if (file.has_end_chunk) {
        w.stop_ns = file.stop_ns;
    }

    w.slice_by_cpu.resize(file.header.cpu_count);
    w.insert_slice = prepare_statement(db, INSERT_SLICE_SQL, error);
    w.insert_state = prepare_statement(db, INSERT_STATE_SQL, error);
    w.insert_thread_name = prepare_statement(db, INSERT_THREAD_NAME_SQL, error);
    if (!w.insert_slice || !w.insert_state || !w.insert_thread_name) {
        return false;
    }

    collect_activity(w, order);
    mark_session_start_runners(w, order);
    if (!walk_timeline(w, order) || !end_open_rows(w)) {
        return false;
    }

    if (!write_thread_rows(w) || !write_process_rows(w)) {
        return false;
    }

    return execute_sql(db, DERIVED_INDEXES, error);
}
