# ktrace queries

Tested queries for a ktrace-decode database. Open it with `sqlite3 run.db` (3.25 or newer, for
window functions) and attach the lookups from `scripts/build_lookups.py` first:

```sql
ATTACH 'lookups.db' AS lookup;
```

Queries that need a thread's name use this table expression, its latest name:

```sql
WITH thread_label AS (
    SELECT tn.tid, n.name FROM thread_names tn JOIN names n ON n.name_id = tn.name_id
    WHERE tn.last_ns = (SELECT MAX(last_ns) FROM thread_names WHERE tid = tn.tid)
)
SELECT * FROM thread_label LIMIT 5;
```

## Trust and overview

Whether the file holds the whole session. Always first:

```sql
SELECT name, passed, detail FROM health;
```

The session at a glance:

```sql
SELECT arch, cpu_count, (stop_ns - start_ns) / 1e9 AS seconds, records, lost_records, records_before_start
FROM session;
```

## CPU time

How busy each CPU was:

```sql
SELECT c.cpu,
       ROUND(100.0 * SUM(CASE WHEN t.kind != 3 THEN c.end_ns - c.start_ns END) / (s.stop_ns - s.start_ns), 1) AS busy_pct
FROM cpu_slices c JOIN threads t ON t.tid = c.tid, session s
GROUP BY c.cpu;
```

CPU time by process. Kernel tasks belong to no process, so the next query covers them:

```sql
SELECT p.pid, n.name, p.threads, ROUND(SUM(c.end_ns - c.start_ns) / 1e6) AS cpu_ms
FROM processes p
JOIN threads t ON t.pid = p.pid
JOIN cpu_slices c ON c.tid = t.tid
LEFT JOIN names n ON n.name_id = p.leader_name_id
GROUP BY p.pid ORDER BY cpu_ms DESC LIMIT 15;
```

CPU time by kernel task:

```sql
WITH thread_label AS (
    SELECT tn.tid, n.name FROM thread_names tn JOIN names n ON n.name_id = tn.name_id
    WHERE tn.last_ns = (SELECT MAX(last_ns) FROM thread_names WHERE tid = tn.tid)
)
SELECT t.tid, l.name, ROUND(SUM(c.end_ns - c.start_ns) / 1e6, 1) AS cpu_ms
FROM threads t JOIN cpu_slices c ON c.tid = t.tid JOIN thread_label l ON l.tid = t.tid
WHERE t.kind = 2 GROUP BY t.tid ORDER BY cpu_ms DESC;
```

Activity over time, one row per second. A slice counts toward the second it started in:

```sql
SELECT (c.start_ns - s.start_ns) / 1000000000 AS second,
       ROUND(SUM(CASE WHEN t.kind != 3 THEN c.end_ns - c.start_ns END) / 1e9, 2) AS busy_cpu_seconds
FROM cpu_slices c JOIN threads t ON t.tid = c.tid, session s
GROUP BY second ORDER BY second;
```

Threads that ran on more than one CPU:

```sql
SELECT tid, COUNT(DISTINCT cpu) AS cpus FROM cpu_slices GROUP BY tid HAVING cpus > 1;
```

## Thread states

Where one process's threads spent their time. Replace 115 with the pid to inspect:

```sql
SELECT ts.tid, ts.state, COUNT(*) AS stretches, ROUND(SUM(ts.end_ns - ts.start_ns) / 1e6) AS ms
FROM thread_states ts JOIN threads t ON t.tid = ts.tid
WHERE t.pid = 115
GROUP BY ts.tid, ts.state ORDER BY ts.tid, ts.state;
```

How long woken threads waited for a CPU:

```sql
WITH waits AS (
    SELECT end_ns - start_ns AS ns, ROW_NUMBER() OVER (ORDER BY end_ns - start_ns) AS n, COUNT(*) OVER () AS total
    FROM thread_states WHERE state = 'runnable' AND waker_tid IS NOT NULL AND end_ns IS NOT NULL
)
SELECT MAX(total) AS waits,
       MAX(CASE WHEN n = total / 2 THEN ns END) / 1e3 AS p50_us,
       MAX(CASE WHEN n = total * 99 / 100 THEN ns END) / 1e3 AS p99_us,
       MAX(ns) / 1e3 AS max_us,
       SUM(ns > 1e6) AS over_1ms
FROM waits;
```

Waits over 1 ms during which another CPU was idle when the thread was woken:

```sql
WITH long_waits AS (
    SELECT ts.tid, ts.start_ns, ts.end_ns - ts.start_ns AS ns,
           (SELECT cpu FROM cpu_slices WHERE tid = ts.tid AND start_ns = ts.end_ns) AS ran_on
    FROM thread_states ts
    WHERE ts.state = 'runnable' AND ts.waker_tid IS NOT NULL AND ts.end_ns - ts.start_ns > 1e6
),
checked AS (
    SELECT w.tid, w.start_ns, w.ns,
           MAX((SELECT t.kind FROM cpu_slices c JOIN threads t ON t.tid = c.tid
                WHERE c.cpu = k.cpu AND c.start_ns <= w.start_ns ORDER BY c.start_ns DESC LIMIT 1) = 3) AS other_cpu_idle
    FROM long_waits w JOIN cpus k ON k.cpu != w.ran_on
    GROUP BY w.tid, w.start_ns
)
SELECT COUNT(*) AS waits_over_1ms, SUM(other_cpu_idle) AS with_another_cpu_idle,
       ROUND(SUM(ns) / 1e9, 2) AS wait_s, ROUND(SUM(CASE WHEN other_cpu_idle THEN ns END) / 1e9, 2) AS wait_s_with_idle_cpu
FROM checked;
```

What sleeping threads were waiting in. A sleep still running at the stop has no recorded syscall:

```sql
SELECT COALESCE(sn.name, CASE WHEN ts.in_page_fault THEN 'page fault' ELSE 'no recorded syscall' END) AS slept_in,
       COUNT(*) AS sleeps, ROUND(SUM(ts.end_ns - ts.start_ns) / 1e9, 1) AS thread_seconds
FROM thread_states ts LEFT JOIN lookup.syscall_names sn ON sn.number = ts.syscall_number
WHERE ts.state = 'sleeping' GROUP BY slept_in ORDER BY thread_seconds DESC LIMIT 10;
```

Who wakes whom. User threads show as their process, kernel tasks by name:

```sql
WITH thread_label AS (
    SELECT tn.tid, n.name FROM thread_names tn JOIN names n ON n.name_id = tn.name_id
    WHERE tn.last_ns = (SELECT MAX(last_ns) FROM thread_names WHERE tid = tn.tid)
),
label AS (
    SELECT t.tid, CASE WHEN t.kind = 1 THEN 'pid ' || t.pid ELSE l.name END AS who
    FROM threads t JOIN thread_label l ON l.tid = t.tid
)
SELECT a.who AS waker, b.who AS woken, COUNT(*) AS wakeups
FROM sched_wakeup w JOIN label a ON a.tid = w.waker_tid JOIN label b ON b.tid = w.woken_tid
GROUP BY waker, woken ORDER BY wakeups DESC LIMIT 15;
```

## Syscalls

Syscalls by count, with their time inside the kernel and failures:

```sql
SELECT sn.name, COUNT(*) AS calls, ROUND(SUM(s.duration_ns) / 1e6) AS wall_ms,
       SUM(s.result BETWEEN -4095 AND -1) AS failed
FROM syscall s LEFT JOIN lookup.syscall_names sn ON sn.number = s.number
GROUP BY s.number ORDER BY calls DESC LIMIT 15;
```

Failing syscalls by error. A result from -4095 to -1 is a negated errno:

```sql
SELECT sn.name AS syscall, en.name AS error, COUNT(*) AS calls
FROM syscall s
LEFT JOIN lookup.syscall_names sn ON sn.number = s.number
LEFT JOIN lookup.errno_names en ON en.errno = -s.result
WHERE s.result BETWEEN -4095 AND -1
GROUP BY s.number, s.result ORDER BY calls DESC LIMIT 20;
```

Process creations and the process each created. Thread ids only increase, so the created process
is the first to appear after the call returns with a pid above every thread seen before it:

```sql
WITH creations AS (
    SELECT s.ts_ns - s.duration_ns AS call_ns, s.ts_ns AS return_ns, s.duration_ns, s.pid AS caller_pid
    FROM syscall s JOIN lookup.syscall_names sn ON sn.number = s.number WHERE sn.name = 'proc_create'
)
SELECT ROUND((c.call_ns - x.start_ns) / 1e9, 3) AS at_s, ROUND(c.duration_ns / 1e6, 1) AS create_ms, c.caller_pid,
       (SELECT p.pid FROM processes p
        WHERE p.first_ns >= c.return_ns AND p.pid > (SELECT MAX(tid) FROM threads WHERE first_ns < c.call_ns)
        ORDER BY p.first_ns LIMIT 1) AS created_pid
FROM creations c, session x ORDER BY c.call_ns;
```

## Page faults

Page faults by process and access. A nonzero result is a fault the kernel refused:

```sql
SELECT n.name || ' ' || t.pid AS process,
       CASE WHEN f.flags & 4 THEN 'instruction' WHEN f.flags & 2 THEN 'write' ELSE 'read' END AS access,
       COUNT(*) AS faults, ROUND(SUM(f.duration_ns) / 1e6) AS wall_ms, SUM(f.result != 0) AS refused
FROM page_fault f JOIN threads t ON t.tid = f.tid
JOIN processes p ON p.pid = t.pid LEFT JOIN names n ON n.name_id = p.leader_name_id
GROUP BY t.pid, access ORDER BY faults DESC LIMIT 15;
```

Faults inside a syscall, whose time that syscall's duration already includes:

```sql
SELECT COUNT(*) AS faults_inside_syscalls, ROUND(SUM(f.duration_ns) / 1e6) AS ms
FROM page_fault f
WHERE (SELECT s.ts_ns - s.duration_ns FROM syscall s
       WHERE s.tid = f.tid AND s.ts_ns >= f.ts_ns ORDER BY s.ts_ns LIMIT 1) <= f.ts_ns - f.duration_ns;
```

Faults during which the thread slept, and its time asleep:

```sql
SELECT t.pid, COUNT(*) AS sleeps_in_faults, ROUND(SUM(ts.end_ns - ts.start_ns) / 1e6) AS ms
FROM thread_states ts JOIN threads t ON t.tid = ts.tid
WHERE ts.state = 'sleeping' AND ts.in_page_fault = 1
GROUP BY t.pid ORDER BY ms DESC;
```
