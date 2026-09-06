#!/bin/bash
# Boot one instance with gdb attached and a breakpoint on the panic entry, run a
# shell command, and if the kernel panics freeze EVERY CPU at that instant and
# collect evidence: per-CPU backtraces and control registers, the trap frame,
# page-walk reads and gva2gpa of the fault registers, a symbolized stack walk of
# the faulting context, and optionally a full guest RAM dump. Nothing else runs
# between the fault and the capture, the kernel's own panic text is appended to
# serial.log only after the evidence is saved.
#
# usage: catch_frozen.sh [options] [-- extra qemu args]
#   --arch x86_64|aarch64   default x86_64
#   --image PATH            default images/stellux-<arch>.img under the repo root
#   --kernel ELF            symbols, default build/kernel/<arch>/kernel.elf
#                           MUST be the ELF inside the image, or every address lies
#   --smp N                 default 4
#   --mem SIZE              default 4G
#   --cmd 'text'            shell command, default 'ping 10.0.2.2 30'
#   --done RE               regex marking the command finished, default 'packets transmitted'
#   --break SYMBOL          default panic::on_trap (same name on both architectures)
#   --gdb-port P            default 4700, use distinct ports for parallel catches
#   --dump-ram              save guest RAM to <out>/ram_<start>.bin while frozen
#   --gdb-extra 'cmd'       extra gdb command run while frozen, repeatable
#   --keep                  stay frozen after the capture: interactive gdb when run
#                           from a terminal, otherwise gdb sleeps in the background
#   --boot-timeout S        default 120
#   --cmd-timeout S         default 120
#   --tag NAME              default catch
#   --out DIR               default /tmp/stlx_forensics/<tag>
#
# Prints one line: catch=<tag> booted=.. done=.. frozen=0|1 evidence=<dir>
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/qemu_lib.sh"

ARCH=x86_64; IMAGE=""; KERNEL=""; SMP=4; MEM=4G; CMD='ping 10.0.2.2 30'
DONE_RE='packets transmitted'; BREAK='panic::on_trap'; PORT=4700; DUMP_RAM=0
GDB_EXTRA=""; KEEP=0; BOOT_TO=120; CMD_TO=120; TAG=catch; OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --image) IMAGE="$2"; shift 2 ;;
        --kernel) KERNEL="$2"; shift 2 ;;
        --smp) SMP="$2"; shift 2 ;;
        --mem) MEM="$2"; shift 2 ;;
        --cmd) CMD="$2"; shift 2 ;;
        --done) DONE_RE="$2"; shift 2 ;;
        --break) BREAK="$2"; shift 2 ;;
        --gdb-port) PORT="$2"; shift 2 ;;
        --dump-ram) DUMP_RAM=1; shift ;;
        --gdb-extra) GDB_EXTRA="$GDB_EXTRA
$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --boot-timeout) BOOT_TO="$2"; shift 2 ;;
        --cmd-timeout) CMD_TO="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --) shift; break ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

ROOT=$(stlx_root)
[ -n "$IMAGE" ] || IMAGE="$ROOT/images/stellux-$ARCH.img"
[ -n "$KERNEL" ] || KERNEL="$ROOT/build/kernel/$ARCH/kernel.elf"
[ -f "$IMAGE" ] || { echo "image not found: $IMAGE" >&2; exit 2; }
[ -f "$KERNEL" ] || { echo "kernel ELF not found: $KERNEL" >&2; exit 2; }
[ -n "$OUT" ] || OUT="/tmp/stlx_forensics/$TAG"
rm -rf "$OUT"; mkdir -p "$OUT"

LOG="$OUT/serial.log"; FIFO="$OUT/in"; VARS="$OUT/vars.fd"; MON="$OUT/monitor.sock"
GDBTXT="$OUT/gdb.txt"; CMDS="$OUT/gdb.cmds"
MEM_BYTES=$(stlx_mem_bytes "$MEM")

GDB=gdb
if [ "$ARCH" = "aarch64" ] && command -v gdb-multiarch >/dev/null 2>&1; then GDB=gdb-multiarch; fi

# The gdb script: arm, release the VM, then capture once the breakpoint fires.
# The follow-up file is produced by catch_followup.sh from the serial log at
# that moment, so addresses printed by the panic handler can be dumped too.
{
    echo "set pagination off"
    echo "set confirm off"
    echo "file $KERNEL"
    [ "$ARCH" = "aarch64" ] && echo "set architecture aarch64"
    echo "target remote :$PORT"
    echo "break $BREAK"
    echo "continue"
    echo 'echo \n=== FROZEN AT BREAKPOINT ===\n'
    echo "info threads"
    echo "thread apply all -q -s bt 12"
    if [ "$ARCH" = "x86_64" ]; then
        echo 'echo \n=== cr3 / cr4 per CPU (thread N is CPU N-1) ===\n'
        echo 'thread apply all -q -s print/x $cr3'
        echo 'thread apply all -q -s print/x $cr4'
    fi
    echo 'echo \n=== trap frame of the faulting CPU ===\n'
    echo "print/x *tf"
    echo "set \$arch = \"$ARCH\""
    echo "source $HERE/frozen_evidence.py"
    if [ "$DUMP_RAM" = 1 ]; then
        echo 'echo \n=== guest RAM dump while frozen ===\n'
        stlx_ram_regions "$ARCH" "$MEM_BYTES" | while read -r start size; do
            echo "monitor pmemsave $start $size \"$OUT/ram_$(printf '0x%x' "$start").bin\""
        done
        echo 'echo dumped\n'
    fi
    if [ -n "$GDB_EXTRA" ]; then
        echo 'echo \n=== extra ===\n'
        printf '%s\n' "$GDB_EXTRA"
    fi
    echo 'echo \n=== EVIDENCE DONE ===\n'
    if [ "$KEEP" = 1 ] && [ ! -t 0 ]; then
        echo 'echo VM stays frozen while this gdb lives, inspect through the monitor socket\n'
        echo "python import time; time.sleep(86400)"
    fi
} > "$CMDS"

rm -f "$FIFO"; mkfifo "$FIFO"
: > "$LOG"
stlx_start_qemu "$ARCH" "$SMP" "$MEM" "$IMAGE" "$VARS" "$LOG" "$FIFO" \
        -S -gdb "tcp::$PORT" -monitor "unix:$MON,server,nowait" "$@" || exit 1
QPID="$STLX_QPID"
exec 3>"$FIFO"
sleep 1

if [ "$KEEP" = 1 ] && [ -t 0 ]; then
    # Interactive: the human gets the gdb prompt once the capture is done.
    ( sleep 1
      if stlx_wait_shell "$LOG" 3 "$BOOT_TO"; then sleep 2; printf '%s\r' "$CMD" >&3; fi ) &
    "$GDB" -q -x "$CMDS" 2>&1 | tee "$GDBTXT"
    exec 3>&-
    stlx_stop_qemu "$QPID"
    exit 0
fi

"$GDB" -batch -q -x "$CMDS" > "$GDBTXT" 2>&1 &
GPID=$!

booted=0; done=0; frozen=0
if stlx_wait_shell "$LOG" 3 "$BOOT_TO"; then
    booted=1
    sleep 2
    printf '%s\r' "$CMD" >&3
    i=0
    while [ "$i" -lt "$CMD_TO" ]; do
        if grep -q 'FROZEN AT BREAKPOINT' "$GDBTXT" 2>/dev/null; then frozen=1; break; fi
        if grep -qE "$DONE_RE" "$LOG" 2>/dev/null; then done=1; break; fi
        sleep 1; i=$((i + 1))
    done
elif grep -q 'FROZEN AT BREAKPOINT' "$GDBTXT" 2>/dev/null; then
    frozen=1
fi

if [ "$frozen" = 1 ]; then
    stlx_wait_pattern "$GDBTXT" 'EVIDENCE DONE' 300 >/dev/null
fi

if [ "$frozen" = 1 ] && [ "$KEEP" = 1 ]; then
    echo "catch=$TAG booted=$booted done=$done frozen=1 evidence=$OUT"
    echo "VM frozen. Monitor: nc -U $MON   (try: info registers -a, x /8gx ADDR, gva2gpa ADDR)"
    echo "Release with: kill $GPID $QPID"
    exit 0
fi

exec 3>&-
kill "$GPID" 2>/dev/null; wait "$GPID" 2>/dev/null
# gdb has detached, so the VM resumes and the panic handler prints its report.
[ "$frozen" = 1 ] && sleep 3
stlx_stop_qemu "$QPID"
rm -f "$FIFO" "$VARS" "$MON"
if [ "$frozen" = 0 ]; then rm -f "$GDBTXT" "$CMDS"; fi
echo "catch=$TAG booted=$booted done=$done frozen=$frozen evidence=$([ "$frozen" = 1 ] && echo "$OUT" || echo none)"
