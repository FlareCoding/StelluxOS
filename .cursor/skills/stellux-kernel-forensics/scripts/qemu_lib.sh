#!/bin/bash
# Shared helpers for the forensics scripts. Source this file, do not run it.
# Works with the macOS default bash 3.2 and any newer bash on Linux.

# Repository root, from the current directory or STLX_ROOT.
stlx_root() {
    if [ -n "${STLX_ROOT:-}" ]; then
        echo "$STLX_ROOT"
    else
        git rev-parse --show-toplevel 2>/dev/null || pwd
    fi
}

# First existing file among the arguments, or empty.
stlx_first_existing() {
    local f
    for f in "$@"; do
        if [ -f "$f" ]; then echo "$f"; return 0; fi
    done
    return 1
}

stlx_ovmf_code() {
    stlx_first_existing \
        /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
        /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd \
        /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
        /usr/local/share/qemu/edk2-x86_64-code.fd
}

stlx_ovmf_vars() {
    stlx_first_existing \
        /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd \
        /opt/homebrew/share/qemu/edk2-i386-vars.fd \
        /usr/local/share/qemu/edk2-i386-vars.fd
}

stlx_efi_aarch64() {
    stlx_first_existing \
        /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
        /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
        /usr/local/share/qemu/edk2-aarch64-code.fd
}

# Start one headless instance in the background and set STLX_QPID to its PID.
#   stlx_start_qemu ARCH SMP MEM IMAGE VARS_COPY LOG FIFO [extra qemu args...]
# The image is opened with -snapshot so many instances can share one file.
# VARS_COPY is a writable per-instance copy of the UEFI variable store (x86 only).
# Output is redirected before stdin on purpose: opening the fifo for reading
# blocks until the caller opens its write end, and a pipe still held at that
# point would deadlock a caller that captures output.
stlx_start_qemu() {
    local arch="$1" smp="$2" mem="$3" image="$4" vars="$5" log="$6" fifo="$7"
    shift 7

    if [ "$arch" = "x86_64" ]; then
        local code
        code=$(stlx_ovmf_code) || { echo "OVMF firmware not found" >&2; return 1; }
        cp "$(stlx_ovmf_vars)" "$vars"
        qemu-system-x86_64 \
            -machine q35 -cpu qemu64,+fsgsbase,+rdrand -m "$mem" -smp "$smp" \
            -drive if=pflash,format=raw,readonly=on,file="$code" \
            -drive if=pflash,format=raw,file="$vars" \
            -drive format=raw,file="$image" -snapshot \
            -device qemu-xhci,id=xhci -device usb-hub,bus=xhci.0,port=1 \
            -device usb-kbd,bus=xhci.0,port=1.1 -device usb-mouse,bus=xhci.0,port=1.2 \
            -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
            -nographic -no-reboot -no-shutdown \
            "$@" > "$log" 2>&1 < "$fifo" &
    else
        local efi
        efi=$(stlx_efi_aarch64) || { echo "AArch64 EFI firmware not found" >&2; return 1; }
        qemu-system-aarch64 \
            -machine virt,gic-version=2 -cpu cortex-a57 -m "$mem" -smp "$smp" \
            -bios "$efi" \
            -drive format=raw,file="$image" -snapshot \
            -device qemu-xhci,id=xhci -device usb-hub,bus=xhci.0,port=1 \
            -device usb-kbd,bus=xhci.0,port=1.1 -device usb-mouse,bus=xhci.0,port=1.2 \
            -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
            -nographic -no-reboot -no-shutdown \
            "$@" > "$log" 2>&1 < "$fifo" &
    fi
    STLX_QPID=$!
}

# Poll LOG until REGEX (extended) appears or SECONDS elapse. Returns 0 when found.
stlx_wait_pattern() {
    local log="$1" re="$2" secs="$3" i
    for i in $(seq 1 "$secs"); do
        if grep -qE "$re" "$log" 2>/dev/null; then return 0; fi
        sleep 1
    done
    return 1
}

# Poll LOG until either regex appears. Prints "first", "second", or "timeout".
stlx_wait_either() {
    local log="$1" re1="$2" re2="$3" secs="$4" i
    for i in $(seq 1 "$secs"); do
        if grep -qE "$re1" "$log" 2>/dev/null; then echo first; return 0; fi
        if grep -qE "$re2" "$log" 2>/dev/null; then echo second; return 0; fi
        sleep 1
    done
    echo timeout
    return 1
}

# Stop a QEMU instance quietly.
stlx_stop_qemu() {
    local pid="$1"
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}

# Send one HMP command through a monitor unix socket and print the reply with
# the line-editing echo stripped. File arguments must be double quoted inside
# CMD, the HMP expression parser otherwise consumes a leading slash.
#   stlx_hmp SOCKET "cmd args" [seconds to wait for the reply]
stlx_hmp() {
    local sock="$1" cmd="$2" secs="${3:-2}"
    ( printf '%s\n' "$cmd"; sleep "$secs" ) | nc -U "$sock" 2>/dev/null \
        | tr -d '\r' | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -vE '^\(qemu\) |^QEMU .* monitor|^$'
}

# Guest RAM regions for pmemsave as "start size" pairs, one per line.
#   stlx_ram_regions ARCH MEM_BYTES
# q35 places up to 2 GB below 4 GB and the rest from 4 GB up. virt starts at 1 GB.
stlx_ram_regions() {
    local arch="$1" mem="$2"
    if [ "$arch" = "x86_64" ]; then
        local low=$mem
        if [ "$mem" -gt 2147483648 ]; then low=2147483648; fi
        echo "0 $low"
        if [ "$mem" -gt 2147483648 ]; then
            echo "4294967296 $(( mem - 2147483648 ))"
        fi
    else
        echo "1073741824 $mem"
    fi
}

# Parse a QEMU memory size like 4G, 512M, 2048 (MiB) into bytes.
stlx_mem_bytes() {
    local m="$1"
    case "$m" in
        *G|*g) echo $(( ${m%?} * 1024 * 1024 * 1024 )) ;;
        *M|*m) echo $(( ${m%?} * 1024 * 1024 )) ;;
        *) echo $(( m * 1024 * 1024 )) ;;
    esac
}
