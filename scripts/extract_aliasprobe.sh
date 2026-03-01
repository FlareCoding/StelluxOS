#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <serial-log-file>"
    exit 1
fi

LOG_FILE="$1"
if [[ ! -f "$LOG_FILE" ]]; then
    echo "Error: file not found: $LOG_FILE"
    exit 1
fi

echo "=== Alias probe summary from: $LOG_FILE ==="
echo ""

show_marker() {
    local marker="$1"
    local line
    line="$(rg -m1 "$marker" "$LOG_FILE" || true)"
    if [[ -n "$line" ]]; then
        echo "[FOUND] $line"
    else
        echo "[MISSING] $marker"
    fi
}

show_marker "sched: aliasprobe\\[pre_handles\\]"
show_marker "sched: aliasprobe\\[post_handles\\]"
show_marker "sched: aliasprobe task_base_pre"
show_marker "sched: aliasprobe task_off_ff8_pre"
show_marker "sched: aliasprobe task_base_post"
show_marker "sched: aliasprobe task_off_ff8_post"

echo ""
echo "=== Optional panic lines (if present) ==="
rg -n "KERNEL PANIC|Faulting address|ESR:|TTBR0 walk|PAR_EL1|Data Abort|sched: enqueue tid=0 name=\\(null\\)" "$LOG_FILE" || true

echo ""
echo "=== Heuristic checks ==="
pre_line="$(rg -m1 "sched: aliasprobe\\[pre_handles\\]" "$LOG_FILE" || true)"
post_line="$(rg -m1 "sched: aliasprobe\\[post_handles\\]" "$LOG_FILE" || true)"

if [[ -n "$pre_line" && "$pre_line" == *"valid=yes"* ]]; then
    echo "[OK] pre_handles reports valid=yes"
elif [[ -n "$pre_line" ]]; then
    echo "[WARN] pre_handles did not report valid=yes"
else
    echo "[WARN] pre_handles line missing"
fi

if [[ -n "$post_line" && "$post_line" == *"valid=yes"* ]]; then
    echo "[OK] post_handles reports valid=yes"
elif [[ -n "$post_line" ]]; then
    echo "[WARN] post_handles did not report valid=yes"
else
    echo "[WARN] post_handles line missing"
fi

probe_mismatch_count="$(rg -c "task_off_ff8_(pre|post).*fault=yes" "$LOG_FILE" || true)"
if [[ "${probe_mismatch_count:-0}" -eq 0 ]]; then
    echo "[OK] no task_off_ff8 fault=yes probes"
else
    echo "[WARN] found $probe_mismatch_count task_off_ff8 probes with fault=yes"
fi
