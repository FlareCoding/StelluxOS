#!/bin/bash
# Boot many headless StelluxOS instances, run one shell command in each, and
# classify every run. This measures how often an intermittent failure occurs
# and whether a change made it disappear.
#
# usage: stress.sh [options] [-- extra qemu args]
#   --arch x86_64|aarch64   default x86_64
#   --image PATH            default images/stellux-<arch>.img under the repo root
#   --smp N                 default 4
#   --mem SIZE              default 4G
#   --runs N                default 8
#   --parallel N            default 4 instances at a time
#   --cmd 'text'            shell command typed at the prompt, default 'ping 10.0.2.2 20'
#   --done RE               regex marking the command finished, default 'packets transmitted'
#   --fail RE               regex marking a failed run, default 'KERNEL PANIC|\[FATAL\]'
#   --count RE              regex counted per run for the summary, default 'bytes from'
#   --boot-timeout S        default 120
#   --cmd-timeout S         default 90
#   --tag NAME              default stress
#   --out DIR               default /tmp/stlx_forensics/<tag>
#   --expect-clean          exit 1 if any run failed or did not boot
#
# Each run writes <out>/run_<tag>_<n>.log (raw serial) and one summary line.
# The image is never written to (-snapshot), so instances can share it and the
# tree stays untouched.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/qemu_lib.sh"

ARCH=x86_64; IMAGE=""; SMP=4; MEM=4G; RUNS=8; PAR=4
CMD='ping 10.0.2.2 20'; DONE_RE='packets transmitted'; FAIL_RE='KERNEL PANIC|\[FATAL\]'
COUNT_RE='bytes from'; BOOT_TO=120; CMD_TO=90; TAG=stress; OUT=""; EXPECT_CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --image) IMAGE="$2"; shift 2 ;;
        --smp) SMP="$2"; shift 2 ;;
        --mem) MEM="$2"; shift 2 ;;
        --runs) RUNS="$2"; shift 2 ;;
        --parallel) PAR="$2"; shift 2 ;;
        --cmd) CMD="$2"; shift 2 ;;
        --done) DONE_RE="$2"; shift 2 ;;
        --fail) FAIL_RE="$2"; shift 2 ;;
        --count) COUNT_RE="$2"; shift 2 ;;
        --boot-timeout) BOOT_TO="$2"; shift 2 ;;
        --cmd-timeout) CMD_TO="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --expect-clean) EXPECT_CLEAN=1; shift ;;
        --) shift; break ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

ROOT=$(stlx_root)
[ -n "$IMAGE" ] || IMAGE="$ROOT/images/stellux-$ARCH.img"
[ -f "$IMAGE" ] || { echo "image not found: $IMAGE" >&2; exit 2; }
[ -n "$OUT" ] || OUT="/tmp/stlx_forensics/$TAG"
mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"
PROMPT_RE="${STLX_PROMPT_RE:-/ \\\$}"

run_one() {
    local id="$1"; shift
    local log="$OUT/run_${TAG}_${id}.log" fifo="$OUT/in_${id}" vars="$OUT/vars_${id}.fd"
    rm -f "$fifo"; mkfifo "$fifo"
    : > "$log"

    stlx_start_qemu "$ARCH" "$SMP" "$MEM" "$IMAGE" "$vars" "$log" "$fifo" "$@" || return
    local qpid="$STLX_QPID"
    exec 3>"$fifo"

    local booted=0 done=0 failed=0
    if stlx_wait_pattern "$log" "$PROMPT_RE" "$BOOT_TO"; then
        booted=1
        sleep 2
        printf '%s\r' "$CMD" >&3
        case "$(stlx_wait_either "$log" "$DONE_RE" "$FAIL_RE" "$CMD_TO")" in
            first) done=1 ;;
            second) failed=1; sleep 2 ;;
        esac
    elif grep -qE "$FAIL_RE" "$log"; then
        failed=1
    fi

    exec 3>&-
    stlx_stop_qemu "$qpid"
    rm -f "$fifo" "$vars"

    local count
    count=$(grep -cE "$COUNT_RE" "$log" 2>/dev/null || true)
    if grep -qE "$FAIL_RE" "$log"; then failed=1; fi
    echo "run=${TAG}_${id} arch=$ARCH smp=$SMP booted=$booted done=$done failed=$failed count=$count log=$log" >> "$SUMMARY"
}

i=0
while [ "$i" -lt "$RUNS" ]; do
    pids=""
    j=0
    while [ "$j" -lt "$PAR" ] && [ "$i" -lt "$RUNS" ]; do
        run_one "$i" "$@" &
        pids="$pids $!"
        i=$((i + 1)); j=$((j + 1))
        sleep 2
    done
    wait $pids
done

sort -t_ -k2 -n "$SUMMARY"
FAILING=$(grep -c 'failed=1' "$SUMMARY")
CLEAN=$(grep -c 'booted=1 done=1 failed=0' "$SUMMARY")
NOBOOT=$(grep -c 'booted=0' "$SUMMARY")
echo "=== $TAG: arch=$ARCH smp=$SMP runs=$RUNS cmd='$CMD' ==="
echo "failing=$FAILING clean=$CLEAN no_boot=$NOBOOT (logs in $OUT)"

if [ "$EXPECT_CLEAN" = 1 ] && { [ "$FAILING" -gt 0 ] || [ "$NOBOOT" -gt 0 ]; }; then
    exit 1
fi
exit 0
