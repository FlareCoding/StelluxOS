#!/usr/bin/env python3
"""
Decode a Stellux ktrace binary dump (collected via `ktrace --dump`) into
Chrome Trace Format JSON, loadable in chrome://tracing or ui.perfetto.dev.

The decoder is structured as parse -> model -> serialize so that an
alternative serializer (e.g. Perfetto protobuf) can be added later without
touching the parsing or the thread-state reconstruction.

Wire format v2 (little-endian), produced by kernel/trace/trace.cpp:

    header (40 bytes, packed):
        magic[8]   "STLXTRC\\0"
        version    u32        (2)
        arch       u32        (1 = x86_64, 2 = aarch64)
        freq_hz    u64        (timestamp counter frequency)
        cpu_count  u32
        _pad       u32
        total_size u64        (== file size)
    count table:  cpu_count x { cpu_id u32, record_count u32 }
    records:      per cpu, record_count x 48-byte record
        ts u64, name_id u64, arg0 u64, arg1 u64,
        tid u32, pid u32, cat u16, ph u8, fl u8, _pad u32
    string table: name_count u32, then name_count x { len u16, bytes[len] }

Phases ('ph'): 'X' duration span (arg0 = duration cycles), 'i' instant
(used for sched:switch / sched:wakeup), 'C' counter (arg0 = value).
sched:switch: tid = prev, arg0 = next_tid, arg1 = prev_state, fl & idle if
next is the idle task. sched:wakeup: tid = wakee, arg0 = waker_tid.

Usage:
    ./ktrace_decode.py run.stlxtr -o run.json
    ./ktrace_decode.py run.stlxtr            # writes to stdout
"""

import argparse
import json
import struct
import sys
from collections import defaultdict

MAGIC = b"STLXTRC\x00"
SUPPORTED_VERSION = 2

HEADER = struct.Struct("<8sIIQIIQ")     # magic, version, arch, freq, cpus, _pad, total
CPU_ENT = struct.Struct("<II")          # cpu_id, record_count
RECORD = struct.Struct("<QQQQIIHBBI")   # ts, name_id, arg0, arg1, tid, pid, cat, ph, fl, _pad

ARCH_NAMES = {1: "x86_64", 2: "aarch64"}

# Category bitmask -> name (kernel/trace/trace_categories.h)
CATEGORY_NAMES = {
    1 << 0: "boot",
    1 << 1: "syscall",
    1 << 2: "sched",
    1 << 3: "mm",
    1 << 4: "irq",
    1 << 5: "fs",
}

# flags bits (kernel/trace/trace_internal.h)
FLAG_PRIVILEGED = 1 << 0
FLAG_NMI = 1 << 1
FLAG_IDLE = 1 << 2

# task states (kernel/sched/task.h) -> Perfetto-style state names
STATE_NAME = {1: "Runnable", 3: "Sleeping", 4: "Dead"}

# Lane namespaces kept clear of real (small) tids/pids.
CPU_LANE_PID = 1_000_000   # per-CPU "what ran" lanes
STATE_TID_OFF = 1_000_000  # per-thread state lane = STATE_TID_OFF + tid


def category_name(cat):
    if cat == 0:
        return "none"
    parts = [name for bit, name in CATEGORY_NAMES.items() if cat & bit]
    return "|".join(parts) if parts else f"0x{cat:x}"


# ---------------------------------------------------------------- parse

def parse(data):
    if len(data) < HEADER.size:
        raise ValueError("file too small to contain a header")

    magic, version, arch, freq, cpu_count, _pad, total = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r} (expected {MAGIC!r})")
    if version != SUPPORTED_VERSION:
        raise ValueError(f"unsupported dump version {version} (expected {SUPPORTED_VERSION})")
    if total != len(data):
        print(f"warning: header total_size={total} but file is {len(data)} bytes",
              file=sys.stderr)
    if freq == 0:
        print("warning: freq_hz is 0; timestamps left in raw cycles", file=sys.stderr)

    off = HEADER.size
    cpu_meta = []
    for _ in range(cpu_count):
        if off + CPU_ENT.size > len(data):
            break
        cid, cnt = CPU_ENT.unpack_from(data, off)
        off += CPU_ENT.size
        cpu_meta.append((cid, cnt))

    # Tolerate a short/truncated file (e.g. a transfer that was cut off): decode
    # every whole record present and stop cleanly at the first partial one.
    truncated = len(cpu_meta) < cpu_count
    events = []
    for cid, cnt in cpu_meta:
        for _ in range(cnt):
            if off + RECORD.size > len(data):
                truncated = True
                break
            ts, nid, a0, a1, tid, pid, cat, ph, fl, _p = RECORD.unpack_from(data, off)
            off += RECORD.size
            events.append({"cpu": cid, "ts": ts, "nid": nid, "a0": a0, "a1": a1,
                           "tid": tid, "pid": pid, "cat": cat, "ph": chr(ph), "fl": fl})
        if truncated:
            break
    if truncated:
        print("warning: file looks truncated; decoded the records that arrived",
              file=sys.stderr)

    names = []
    if off + 4 <= len(data):
        (name_count,) = struct.unpack_from("<I", data, off)
        off += 4
        for _ in range(name_count):
            if off + 2 > len(data):
                break
            (ln,) = struct.unpack_from("<H", data, off)
            off += 2
            names.append(data[off:off + ln].decode("utf-8", "replace"))
            off += ln

    return {"version": version, "arch": ARCH_NAMES.get(arch, f"arch{arch}"),
            "freq": freq, "events": events, "names": names}


# ---------------------------------------------------------------- model

def _overlap_sum(intervals, a, b):
    total = 0.0
    for (s, e) in intervals:
        if e <= a:
            continue
        if s >= b:
            break
        total += min(e, b) - max(s, a)
    return total


def build_model(parsed):
    freq = parsed["freq"]
    names = parsed["names"]
    events = parsed["events"]

    def name_of(nid):
        return names[nid] if nid < len(names) else f"<id {nid}>"

    min_ts = min((e["ts"] for e in events), default=0)

    def us(cycles):
        return float(cycles) if freq == 0 else (cycles * 1_000_000.0) / freq

    for e in events:
        e["name"] = name_of(e["nid"])
        e["t"] = us(e["ts"] - min_ts)

    tid_pid = {}
    for e in events:
        tid_pid.setdefault(e["tid"], e["pid"])

    spans = [e for e in events if e["ph"] == "X"]
    for s in spans:
        s["dur_us"] = us(s["a0"])  # arg0 = duration in cycles

    other = [e for e in events if e["ph"] in ("i", "C")
             and e["name"] not in ("sched:switch", "sched:wakeup")]

    sched = sorted((e for e in events if e["name"] in ("sched:switch", "sched:wakeup")),
                   key=lambda e: e["t"])
    idle_tids = set(e["a0"] for e in sched
                    if e["name"] == "sched:switch" and (e["fl"] & FLAG_IDLE))

    end_us = max((s["t"] + s["dur_us"] for s in spans), default=0.0)
    end_us = max([end_us] + [e["t"] for e in events]) if events else 0.0

    # Per-thread state intervals from the sched event stream.
    states = defaultdict(list)  # tid -> [(state, start, end)]
    cur = {}                    # tid -> (state, start)

    def close(tid, t):
        if tid in cur:
            st, start = cur.pop(tid)
            if t > start:
                states[tid].append((st, start, t))

    def set_state(tid, st, t):
        close(tid, t)
        cur[tid] = (st, t)

    for e in sched:
        t = e["t"]
        if e["name"] == "sched:wakeup":
            set_state(e["tid"], "Runnable", t)
        else:  # sched:switch
            set_state(e["tid"], STATE_NAME.get(e["a1"], "Runnable"), t)  # prev
            if not (e["fl"] & FLAG_IDLE):
                set_state(e["a0"], "Running", t)                          # next
    for tid in list(cur.keys()):
        close(tid, end_us)
    for tid in idle_tids:
        states.pop(tid, None)

    # Per-CPU "what ran" intervals from per-CPU switch sequences.
    cpu_runs = defaultdict(list)  # cpu -> [(start, end, tid)]
    sw_by_cpu = defaultdict(list)
    for e in sched:
        if e["name"] == "sched:switch":
            sw_by_cpu[e["cpu"]].append(e)
    for cpu, sws in sw_by_cpu.items():
        sws.sort(key=lambda e: e["t"])
        for i, e in enumerate(sws):
            start = e["t"]
            end = sws[i + 1]["t"] if i + 1 < len(sws) else end_us
            if not (e["fl"] & FLAG_IDLE) and end > start:
                cpu_runs[cpu].append((start, end, e["a0"]))

    # Per-span on/off-CPU split from each thread's Running intervals.
    running = {}
    for tid, ivs in states.items():
        running[tid] = sorted((s, e) for (st, s, e) in ivs if st == "Running")
    for s in spans:
        on = _overlap_sum(running.get(s["tid"], []), s["t"], s["t"] + s["dur_us"])
        s["on_cpu_us"] = on
        s["off_cpu_us"] = max(0.0, s["dur_us"] - on)

    return {"freq": freq, "arch": parsed["arch"], "spans": spans, "other": other,
            "states": states, "cpu_runs": cpu_runs, "tid_pid": tid_pid,
            "idle_tids": idle_tids}


# ---------------------------------------------------------------- serialize: chrome

def to_chrome(model):
    ev = []
    procs = set()

    for s in model["spans"]:
        procs.add(s["pid"])
        ev.append({"name": s["name"], "cat": category_name(s["cat"]), "ph": "X",
                   "ts": s["t"], "dur": s["dur_us"], "pid": s["pid"], "tid": s["tid"],
                   "args": {"on_cpu_us": round(s["on_cpu_us"], 3),
                            "off_cpu_us": round(s["off_cpu_us"], 3)}})

    for e in model["other"]:
        procs.add(e["pid"])
        if e["ph"] == "C":
            ev.append({"name": e["name"], "cat": category_name(e["cat"]), "ph": "C",
                       "ts": e["t"], "pid": e["pid"], "args": {"value": e["a0"]}})
        else:  # instant
            ev.append({"name": e["name"], "cat": category_name(e["cat"]), "ph": "i",
                       "ts": e["t"], "pid": e["pid"], "tid": e["tid"], "s": "t"})

    state_lanes = {}  # (pid, lane_tid) -> source tid
    for tid, ivs in model["states"].items():
        pid = model["tid_pid"].get(tid, 0)
        procs.add(pid)
        lane = STATE_TID_OFF + tid
        state_lanes[(pid, lane)] = tid
        for (st, start, end) in ivs:
            ev.append({"name": st, "cat": "state", "ph": "X",
                       "ts": start, "dur": end - start, "pid": pid, "tid": lane})

    for cpu, runs in model["cpu_runs"].items():
        cpu_pid = CPU_LANE_PID + cpu
        for (start, end, run_tid) in runs:
            ev.append({"name": f"tid {run_tid}", "cat": "cpu", "ph": "X",
                       "ts": start, "dur": end - start, "pid": cpu_pid, "tid": 0})

    # Metadata: process + thread + CPU lane names.
    for pid in sorted(procs):
        ev.append({"name": "process_name", "ph": "M", "pid": pid, "tid": 0,
                   "args": {"name": "kernel" if pid == 0 else f"process {pid}"}})
    for cpu in sorted(model["cpu_runs"].keys()):
        ev.append({"name": "process_name", "ph": "M", "pid": CPU_LANE_PID + cpu,
                   "tid": 0, "args": {"name": f"CPU {cpu}"}})
    for (pid, tid) in sorted({(s["pid"], s["tid"]) for s in model["spans"]}):
        ev.append({"name": "thread_name", "ph": "M", "pid": pid, "tid": tid,
                   "args": {"name": f"tid {tid}"}})
    for (pid, lane), tid in state_lanes.items():
        ev.append({"name": "thread_name", "ph": "M", "pid": pid, "tid": lane,
                   "args": {"name": f"tid {tid} state"}})

    return {"traceEvents": ev, "displayTimeUnit": "ms"}


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="Decode a Stellux ktrace dump to Chrome Trace JSON")
    ap.add_argument("input", help="path to the .stlxtr binary dump")
    ap.add_argument("-o", "--output", help="output JSON path (default: stdout)")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    parsed = parse(data)
    model = build_model(parsed)

    n_states = sum(len(v) for v in model["states"].values())
    n_runs = sum(len(v) for v in model["cpu_runs"].values())
    print(f"ktrace: {parsed['arch']}, {len(parsed['events'])} records, "
          f"{len(model['spans'])} spans, {len(model['states'])} threads with state, "
          f"{n_states} state slices, {n_runs} cpu-run slices, "
          f"freq={parsed['freq'] / 1e9:.3f} GHz", file=sys.stderr)

    chrome = to_chrome(model)

    if args.output:
        with open(args.output, "w") as f:
            json.dump(chrome, f)
        print(f"wrote {args.output} ({len(chrome['traceEvents'])} events)", file=sys.stderr)
    else:
        json.dump(chrome, sys.stdout)


if __name__ == "__main__":
    main()
