# ktrace database reference

Every table and column of a ktrace-decode database. Many columns also carry a note in the schema,
which `.schema TABLE` shows, but the payload columns have none.

## Conventions

- Times are integer nanoseconds since boot. The one exception is `session.boot_unix_ns`, the
  wall-clock time at boot.
- A record's `ts_ns` is when the kernel wrote it. Syscall and page fault records are written as
  the work ends, so the end is `ts_ns` and the start is `ts_ns - duration_ns`. That start comes out
  slightly late, by the moment between measuring the duration and writing the record.
- Event table rows are in timestamp order, so rowid order is time order. A tie goes to the lower CPU.
- NULL means the records cannot determine the value. It never means zero.
- SQLite integers are signed 64-bit, so an unsigned value of 2^63 or more reads back negative.
  This applies to `session.event_mask`, where -1 means every bit is set, and to high addresses.
- Thread names are at most 16 bytes, as the kernel copies them (`ThreadPoolSingl`), and threads
  rename themselves. `thread_names` holds each name a thread had.

## Session tables

`session`, one row: `format_version`, `arch` (`x86_64` or `aarch64`, NULL for another),
`elf_machine`, `cpu_count`, `boot_unix_ns` (NULL without a real-time clock), `start_ns`, `stop_ns`
(NULL when the file was cut short), `event_mask` (bit n set when the session recorded event id n),
`records`, `records_before_start` (older than `start_ns`, left out of every table as the format
says), `lost_records` (dropped while a ring was full), `file_bytes`.

`cpus`, one row per CPU: `cpu`, `records`, `records_before_start`, `lost_records`.

`health`, one row per check: `name`, `passed` (1 or 0), `detail`.

| Check | When it fails |
|---|---|
| `file_complete` | The file has no end chunk, so it was cut short. `stop_ns` is NULL, and rows still open where it was cut have a NULL end. Or bytes follow the end chunk, and the decoder ignores them. |
| `no_lost_records` | Rings overflowed. Counts are low and derived rows near the losses are unreliable. |
| `known_event_ids` | Some records have event ids the decoder does not know. They are kept raw in `unknown_records`. |
| `switch_chain` | Records were lost from a CPU's chain of switches. Some `cpu_slices` and running states end in NULL. |

## Event tables

One table per event, generated from the field lists in `kernel/trace/ktrace_format.h`. Every row
starts with `ts_ns` and `cpu`, the CPU whose ring held the record. A `*_name_id` column refers to
`names.name_id`, and `names` maps each id to a name.

`sched_switch`: the CPU switched from `prev_tid` to `next_tid`. `reason` is how the previous task
left, `prev_kind` and `next_kind` are task kinds, and `next_pid` is the pid of the task switched
in, 0 for a task that belongs to no process. `next_pid` only means something when `next_kind` is
not 0.

`sched_wakeup`: `waker_tid` moved `woken_tid` from blocked to ready, queueing it on `target_cpu`.
The waker is the task that was running when the wakeup happened, so in an interrupt it is the
interrupted task, and the idle task when that CPU was idle. A wakeup is recorded only for a task
that was blocked.

`syscall`: `tid` of process `pid` made syscall `number`, which ran for `duration_ns` and returned
`result`. Numbers are the architecture's Linux syscall numbers, plus StelluxOS's own from 1000 up.
A syscall is recorded only when the session was already recording as it began, and dynamic
privilege elevation (`SYS_ELEVATE`, 1001) is never recorded.

`page_fault`: `tid` took a user page fault at `address` with `flags` (codes below). The handler ran
for `duration_ns`, which includes waiting for the address space's lock, and returned `result`, 0
when it resolved the fault. Like syscalls, a fault is recorded only when the session was already
recording as it began.

`unknown_records`: `ts_ns`, `cpu`, `event_id`, and `record`, the whole 64-byte record as a blob.

## Derived tables

These say only what follows from the records. Anything they cannot determine is left out or NULL.

`cpu_slices`: what ran on each CPU. A slice runs from a switch-in to that CPU's next switch. The
stretch before a CPU's first switch belongs to that switch's previous task, from `session.start_ns`.
Idle time appears as slices of the CPU's idle task (`threads.kind = 3`). `end_ns` is NULL when unknown.

`thread_states`: every stretch of a thread's life, for threads that are not idle tasks.

| State | Starts | Ends | Other columns |
|---|---|---|---|
| `running` | its switch-in | its next switch-out | `cpu` |
| `runnable` | a wakeup, or a switch-out preempted (0) or yielded (1) | its next switch-in | `waker_tid` or `switch_reason` |
| `sleeping` | a switch-out with reason blocked | its next wakeup | `syscall_number`, `in_page_fault` |

- A wakeup that arrives while the thread is still running starts nothing. The thread was about to
  sleep, and its switch-out then reads yielded.
- `syscall_number` is NULL when no syscall record covers the moment it slept. Kernel tasks sleep
  outside syscalls, and a syscall still running at the stop has no record, because syscalls are
  recorded as they return.
- One syscall or fault can hold several sleeping stretches, for example a thread that wakes to find
  a lock taken again. Count syscalls and faults from their own tables, never from stretches.
- Stretches before a thread's first record are left out. The one exception is the task each CPU
  was running when the session started, which is running from `session.start_ns`, like its slice.
- Rows still open at the stop end at `stop_ns`, or have a NULL end when the file was cut short.

`threads`: `tid`, `kind` (NULL when no record carries it), `pid` (from its switch-ins, else its
syscalls), `first_ns` and `last_ns` (its first and last record), `exit_ns` (the switch-out with
reason exited, NULL when it did not exit).

`thread_names`: `tid`, `name_id`, and `first_ns` and `last_ns`, the first and last records showing
the thread with that name.

`processes`: `pid`, `leader_name_id` (the latest name of the thread whose tid is the pid),
`threads`, `first_ns`, `last_ns`. Kernel tasks belong to no process and appear only in `threads`.

## Codes

`scripts/build_lookups.py` writes all of these into a lookup database, each from its source.

| Code | Values | Source |
|---|---|---|
| Task kind | 0 not recorded, 1 user, 2 kernel, 3 idle | `TASK_KIND_*`, `kernel/trace/ktrace_format.h` |
| Switch reason | 0 preempted, 1 yielded, 2 blocked, 3 exited | `SWITCH_REASON_*`, same header |
| Fault flags | bits 1 page present (a protection fault), 2 write, 4 instruction fetch | `PF_FLAG_*`, `kernel/mm/mm.h` |
| Fault result | 0 resolved, negative `MM_CTX_ERR_*` codes | `kernel/mm/vma.h` |
| Syscall number | the architecture's Linux numbers, StelluxOS's from 1000 | `userland/sysroot/ARCH/include/bits/syscall.h`, `userland/lib/libstlx/include/stlx/syscall_nums.h` |
| Syscall result | -4095 to -1 is a negated errno, Linux numbering on both architectures | `userland/sysroot/ARCH/include/bits/errno.h` |

## Indexes

| Table | Index |
|---|---|
| Each event table | every column ending in `tid`, together with `ts_ns` |
| `cpu_slices` | `(cpu, start_ns)` and `(tid, start_ns)` |
| `thread_states` | `(tid, start_ns)` |
| `thread_names` | `(tid, first_ns)` |

## Sources in the tree

| Part | Path |
|---|---|
| Format, events and field lists | `kernel/trace/ktrace_format.h` |
| Recording, rings and streaming | `kernel/trace/ktrace.cpp`, `kernel/trace/ktrace_events.cpp` |
| Event hooks | `kernel/sched/sched.cpp`, `kernel/syscall/syscall.cpp`, `kernel/mm/mm.cpp` |
| `ktrace` command | `userland/apps/ktrace/src/ktrace.c` |
| `ktrace-decode` | `userland/apps/ktrace-decode/src/` |
