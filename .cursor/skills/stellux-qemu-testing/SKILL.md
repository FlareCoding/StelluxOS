---
name: stellux-qemu-testing
description: Boot, drive, and visually verify StelluxOS in QEMU from the host shell, including serial control, GUI input injection, screenshots, and known pitfalls. Use when testing StelluxOS changes in QEMU, capturing before/after screenshots, debugging boot behavior, or measuring anything inside the OS.
---

# StelluxOS QEMU Testing

## Boot recipes

Output-only session (most robust, no stdin fragility):

```bash
rm -f /tmp/stlx_serial.log
OVMF_CODE=/opt/homebrew/share/qemu/edk2-x86_64-code.fd
(qemu-system-x86_64 -machine q35 -cpu qemu64,+fsgsbase,+rdrand -m 4G -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=/tmp/stlx_vars.fd \
  -drive format=raw,file=images/stellux-x86_64.img \
  -device qemu-xhci -device usb-kbd -device usb-mouse \
  -serial file:/tmp/stlx_serial.log \
  -monitor unix:/tmp/stlx_mon,server,nowait \
  -display none -no-reboot -no-shutdown > /dev/null 2>&1 & echo $! > /tmp/stlx_pid)
```

Interactive serial session: replace `-serial file:...` with
`-serial stdio < /tmp/stlx_in > /tmp/stlx_out 2>&1` after `mkfifo /tmp/stlx_in`,
then `exec 3>/tmp/stlx_in` and send commands with `printf 'cmd\r' >&3`.

- CRITICAL: the fifo write end dies between separate shell tool calls, which
  EOFs QEMU stdin and kills it. Keep boot plus all serial interaction in ONE
  command chain, or use the output-only recipe with monitor input instead.
- Wait for boot with: `for i in $(seq 1 90); do grep -q '\$ ' /tmp/stlx_out && break; sleep 1; done`
  (with -serial file, grep the log for `stlxdm started`).
- `/tmp/stlx_vars.fd` is a writable copy of `edk2-i386-vars.fd`.

## Driving the GUI via the monitor socket

All through `printf '<cmd>\n' | nc -U /tmp/stlx_mon`:

- `sendkey ret` — dismiss the boot splash (required before the desktop shows).
- `sendkey ctrl-alt-t` — open a terminal via the stlxdm shortcut.
- Type into GUI apps one key at a time: `for k in s t l x t o p ret; do printf "sendkey $k\n" | nc -U /tmp/stlx_mon; sleep 0.2; done` (`spc` = space, `slash` = /).
- Mouse is RELATIVE, cursor starts at screen center (960, 540) native:
  `mouse_move <dx> <dy>`, `mouse_button 1` press, `mouse_button 0` release.
  Taskbar items are centered at y=1056: with 2 items, first at x=940, second x=980.
- `screendump /tmp/shot.png -f png` — native 1920x1080 PNG.

## Screenshots and visual verification

- The image Read tool displays captures scaled to 1024x576: multiply displayed
  coordinates by 1.875 for native crop coordinates.
- Zoom crops for pixel inspection: `magick shot.png -crop WxH+X+Y +repage -filter point -resize 500% out.png`.
- Before/after strips: bordered images appended with `magick a.png b.png +append`
  (montage labels need ghostscript, which is not installed).
- The serial shell echoes every keystroke with `[2K` redraw sequences: NEVER
  measure timing or count prompts from serial output, and filter echo noise
  with `rg -v "2K"` when reading command output.

## Known pitfalls

- Kernel unit tests run on the BSP idle task: `sched::sleep_*` returns
  immediately there and its spinning charges idle time. Tests needing a busy
  task must spawn a finite, self-reporting spinner via `create_kernel_task`
  and place it on another CPU with `enqueue_on`; infinite spinners starve the
  runner and hang the suite.
- Apps do not relink when only a lib archive changed: `touch` the app source
  (or the app's Makefile inputs) after lib-only edits, and confirm with
  `strings initrd/bin/<app> | rg <marker>`.
- pty writes return short by design under load; userland writers must loop.
  poll timeouts are not reliable from userland; pace loops with nanosleep
  against a CLOCK_MONOTONIC deadline and poll input with timeout 0.
- `[WARN] syscall: unimplemented nr=N from tid=T` in serial output is the
  porting feedback loop; nr uses the arch's Linux syscall numbering.
- `make test ARCH=x` does a clean test-flavored rebuild (~60-90s, 611/606
  baseline passes); afterwards run `make clean && make image ARCH=x` so the
  disk image matches committed code before any live verification.
- stlxdm autostarts terminals and dropbear; a serial-shell-launched GUI app
  fails with "failed to create window" until the splash is dismissed.
