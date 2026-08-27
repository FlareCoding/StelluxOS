---
name: stellux-qemu-testing
description: Boot, drive, and visually verify StelluxOS in QEMU from the host shell, including serial control, live command injection, GUI input, screenshots, and known pitfalls. Use when testing StelluxOS changes in QEMU, capturing before/after screenshots, debugging boot behavior, exercising the network stack, or measuring anything inside the OS.
---

# StelluxOS QEMU Testing

Always build the image first, since every recipe boots `images/stellux-<arch>.img`:

```bash
make clean && make image ARCH=x86_64
```

## Boot recipes

Prefer the make targets. They auto-detect UEFI firmware on both Linux and macOS,
so no firmware path belongs in a script.

Output-only session, the simplest reliable form:

```bash
timeout 90 make run-headless ARCH=x86_64 < /dev/null > /tmp/boot.log 2>&1
```

`timeout` returns 124 when it kills QEMU, which is expected, not a failure.
Redirect stdin from /dev/null so the session cannot die on an EOF.

Interactive session that types commands into the serial shell. Boot and all
interaction must stay in ONE shell invocation, because the fifo write end closes
between separate tool calls and that EOFs QEMU:

```bash
rm -f /tmp/stlx_in && mkfifo /tmp/stlx_in
(timeout 100 make run-headless ARCH=x86_64 < /tmp/stlx_in > /tmp/out.log 2>&1 &)
exec 3>/tmp/stlx_in
for i in $(seq 1 60); do grep -q '\$ ' /tmp/out.log 2>/dev/null && break; sleep 1; done
sleep 2
printf 'ping 127.0.0.1\r' >&3
sleep 13
exec 3>&-
grep -E "transmitted|loss" /tmp/out.log
```

Allow roughly 12 seconds per command that produces output over several seconds,
such as ping, and confirm boot by polling the log for the shell prompt rather
than sleeping a fixed amount.

For a graphical session with monitor control, invoke QEMU directly and resolve
firmware the same way the Makefile does:

```bash
OVMF_CODE=$(ls /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
               /opt/homebrew/share/qemu/edk2-x86_64-code.fd 2>/dev/null | head -1)
```

Pass `-monitor unix:/tmp/stlx_mon,server,nowait` and `-display none`, keeping a
writable copy of the vars firmware at /tmp/stlx_vars.fd.

## Driving the GUI via the monitor socket

All through `printf '<cmd>\n' | nc -U /tmp/stlx_mon`:

- `sendkey ret` dismisses the boot splash, required before the desktop shows.
- `sendkey ctrl-alt-t` opens a terminal via the stlxdm shortcut.
- Type into GUI apps one key at a time:
  `for k in s t l x t o p ret; do printf "sendkey $k\n" | nc -U /tmp/stlx_mon; sleep 0.2; done`
  (`spc` is space, `slash` is /).
- Mouse is RELATIVE and starts at screen center (960, 540) native:
  `mouse_move <dx> <dy>`, `mouse_button 1` press, `mouse_button 0` release.
  Taskbar items are centered at y=1056: with two items, first x=940, second x=980.
- `screendump /tmp/shot.png -f png` writes a native 1920x1080 PNG.

## Screenshots and visual verification

- The image Read tool displays captures scaled to 1024x576, so multiply
  displayed coordinates by 1.875 for native crop coordinates.
- Zoom crops for pixel inspection:
  `magick shot.png -crop WxH+X+Y +repage -filter point -resize 500% out.png`
- Before/after strips: bordered images appended with `magick a.png b.png +append`
  (montage labels need ghostscript, which is not installed).

## Verifying a boot is actually healthy

```bash
grep -icE "panic|fatal|unhandled|#GP|page fault" /tmp/boot.log   # expect 0
grep -c "^/ " /tmp/boot.log                                      # shell prompt
```

The shell prompt can interleave with driver log lines on the same line, so match
`/ \$` anywhere rather than anchoring to the start of a line.

## Exercising the network stack

QEMU presents virtio-net, and user-mode networking answers at 10.0.2.2. Use both
targets, since they cover different code paths:

- `ping 127.0.0.1` exercises loopback delivery.
- `ping 10.0.2.2` exercises the real driver TX and RX path.

Both should report `0% packet loss`. Any change touching packet buffers, driver
mappings, or protocol state needs this, not just a quiet boot.

## Known pitfalls

- `rg` is not installed. Use `grep`.
- Kernel unit tests run on the BSP idle task: `sched::sleep_*` returns
  immediately there and its spinning charges idle time. Tests needing a busy
  task must spawn a finite, self-reporting spinner via `create_kernel_task` and
  place it on another CPU with `enqueue_on`. Infinite spinners starve the runner
  and hang the suite.
- Apps do not relink when only a lib archive changed: `touch` the app source
  after lib-only edits, and confirm with `strings initrd/bin/<app> | grep <marker>`.
- pty writes return short by design under load, so userland writers must loop.
  poll timeouts are not reliable from userland, so pace loops with nanosleep
  against a CLOCK_MONOTONIC deadline and poll input with timeout 0.
- The serial shell echoes every keystroke with `[2K` redraw sequences. Never
  measure timing or count prompts from serial output, and filter echo noise with
  `grep -v "2K"` when reading command output.
- `[WARN] syscall: unimplemented nr=N from tid=T` is the porting feedback loop.
  The number uses the architecture's Linux syscall numbering.
- `make test ARCH=x` does a clean test-flavored rebuild taking a minute or two.
  Afterwards run `make clean && make image ARCH=x` so the disk image matches
  committed code before any live verification. Treat the pass count as a
  baseline to compare against the previous run, not a fixed number.
- `make test` does NOT cover userland process, thread, or signal behavior. Any
  change to task lifecycle, exit, clone, or signals must additionally run
  `threadtest`, `pthreadtest`, and `sigtest` live, since only those exercise
  native threads and per-thread exit status.
- A test app that returns to the shell prompt WITHOUT printing its summary line
  has died mid-run, it has not passed. Always grep for the summary
  (`--- Results: N passed` for threadtest, `NAME: N passed` for the others) and
  never infer success from a screen of PASS lines.
- stlxdm autostarts terminals and dropbear, so a serial-shell-launched GUI app
  fails with "failed to create window" until the splash is dismissed.
