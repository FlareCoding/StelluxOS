---
name: stellux-ktrace-analysis
description: Record StelluxOS kernel traces with ktrace, decode them into SQLite with ktrace-decode, and answer performance questions from the database with evidence instead of guesses. Use when asked to profile or explain StelluxOS behavior such as CPU use, scheduling delays, sleeps, syscalls, page faults or process creation, when given a .ktrace file or a ktrace-decode database, or when a claim about a running system needs measured evidence.
---

# StelluxOS Trace Analysis

ktrace records kernel events from every CPU into a `.ktrace` file, and `ktrace-decode` turns the
file into a SQLite database with one table per event, plus derived tables of what ran on each CPU
and what each thread was doing. [reference.md](reference.md) defines every table, column and code,
[queries.md](queries.md) holds tested queries, and `scripts/build_lookups.py` names the raw codes.
Run the script rather than naming codes by hand.

## Principles

- Check `health` before anything else and state its result. reference.md says what each failed
  check makes unreliable.
- Show the query behind every number in a report. Cross-check totals two ways, for example CPU
  time from `cpu_slices` against running time in `thread_states`.
- NULL means the records cannot determine the value. Never count it as zero or fill it in.
- Keep measured facts and hypotheses apart. A hypothesis names the record or experiment that would
  settle it, and "consistent with" never turns into "proven" in the retelling.
- Name processes by leader name and pid as the database shows them. A role such as "the renderer"
  needs evidence in the records, never a guess from the name.
- Keep wall time and CPU time apart. Syscall and fault durations include time asleep, so only
  `cpu_slices` and running states measure CPU time.
- The recorder is in the data. The `ktrace` process streams the rings into the file, so report its
  CPU time and keep it out of conclusions about the workload.
- Read the kernel code at the hook before explaining an event. The trace shows when and where a
  thread waited, and the code shows what could block there.

## Workflow

Copy this checklist and track progress:

```
- [ ] 1 Record: ktrace start -o FILE, run the workload, ktrace stop
- [ ] 2 Decode on StelluxOS: ktrace-decode FILE -o DB
- [ ] 3 Copy the database to the host with scp
- [ ] 4 Read health and session
- [ ] 5 Name the codes: scripts/build_lookups.py DB lookups.db
- [ ] 6 Query: start from queries.md, adapt, cross-check
- [ ] 7 Report: each finding with its query, facts apart from hypotheses
```

**1 Record.** On StelluxOS, `ktrace start -o /tmp/run.ktrace` starts a session that a detached
recorder streams into the file, and `ktrace stop` ends it. `ktrace record -o /tmp/run.ktrace`
records in the foreground until Ctrl-C or `ktrace stop`. Every event type is recorded. Each CPU's
ring holds 65,536 records and is drained every 100 ms, so a CPU that fills its ring between drains
loses records, and `ktrace status` shows each CPU's drained and lost counts while recording. Keep
sessions as short as the question allows. 85 s of Chromium and gcc made 2.7 million records.

**2 Decode.** `ktrace-decode /tmp/run.ktrace -o /tmp/run.db` runs only on StelluxOS. It prints the
session length, row counts, health checks and its own phase times. It exits 0 once the database is
written, even when a health check fails, and 1 on an error. The database is built as `DB.partial`
and renamed when complete, so a leftover `.partial` file is an aborted decode. A trace already on
the host has to be copied into a VM to be decoded.

**3 Copy out.** `make run` forwards host port 2222 to the guest's SSH port, and the desktop starts
dropbear. On the host:

```bash
scp -O -P 2222 root@localhost:/tmp/run.db .
sqlite3 run.db 'PRAGMA integrity_check'
```

StelluxOS has no SFTP server, hence `-O`. The root password comes from the person and is never
printed or written into a file you share. scp's exit status is unreliable here, so trust the
integrity check instead. If SSH does not answer, for example on a headless boot, run
`/bin/dropbear -F -R` on the serial console, which then stays attached to dropbear. Never stop or
reuse the person's QEMU, so run `pgrep -fl qemu-system` first. To boot your own, follow the
stellux-qemu-testing skill with `-snapshot` and a host port other than 2222.

**4 Health.** Run `SELECT * FROM health` and `SELECT * FROM session`. Write down the architecture,
the session length and any lost records before reading anything else.

**5 Name the codes.** Syscall numbers, errno values, task kinds, switch reasons and fault codes are
stored raw:

```bash
python3 .cursor/skills/stellux-ktrace-analysis/scripts/build_lookups.py run.db lookups.db
```

The script reads the architecture from the database and parses each table from the header that
defines the codes, so the names match the tree. It needs that architecture's musl sysroot, which
any userland build produces. Queries then run `ATTACH 'lookups.db' AS lookup`.

**6 Query.** Use the host's `sqlite3`, version 3.25 or newer for window functions. The indexes
pair thread ids with time, so filter on `tid` and time ranges. Take states and CPU time from the
derived tables, and counts and durations from the event tables.

**7 Report.** Lead with the answer. Give each finding its number, its query, and whether it is
measured or a hypothesis. End with what the trace could not answer and the event that would.

## What the trace cannot tell

- Which lock or wait queue a thread slept on. It shows that the thread slept, in which syscall or
  fault, and who woke it.
- Why a wakeup happened, beyond the waking task and its CPU.
- User stacks, function names, or which code ran inside a process.
- Interrupts, disk and network I/O, memory use, and syscall arguments such as paths and sizes.
- Which program a `proc_create` started. queries.md matches each creation to the process that
  appeared next, which is an inference from thread ids.
- Anything before a thread's first record, syscalls and faults that began before the session, and
  syscalls still running at the stop.

For such a question, say the trace does not record it and name the event that would.

## Pitfalls

- The host's errno numbers (macOS) differ from StelluxOS's Linux numbering. Name errors only
  through `lookup.errno_names`.
- x86_64 and aarch64 number syscalls differently. Build lookups for each database, and never reuse
  them across architectures.
- Fault time inside a syscall is part of that syscall's duration, so adding the two counts it
  twice. queries.md finds those faults.
- A sleeping stretch is not a fault or a syscall. One fault can hold several sleeps.
- Almost every yielded switch-out is an idle task handing over its CPU. The rest are `sched_yield`
  calls or threads woken while about to sleep.
- Task names are cut at 16 bytes and change over time. `thread_names` gives the name at a moment.
- `session.event_mask` and addresses of 2^63 or more read back negative.
- The StelluxOS shell has no `;`, so run one command per SSH call. SSH reports exit status 255 even
  on success.
