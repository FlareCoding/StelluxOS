# Raspberry Pi validation checklist (kernel page-table aliasing fix)

Use this checklist when testing on physical Raspberry Pi hardware.

## 1) Build + flash image

1. Build debug image (includes alias probes):
   - `make image ARCH=aarch64 DEBUG=1`
2. Flash `images/stellux-aarch64.img` to the SD card/USB medium.
3. Boot on Pi with serial/console log capture enabled.

## 2) Capture required boot log markers

Collect logs from boot start through first userspace transition (or panic).

### Required markers

- `sched: aliasprobe[pre_handles] ...`
- `sched: aliasprobe[post_handles] ...`
- `sched: aliasprobe task_base_pre ...`
- `sched: aliasprobe task_off_ff8_pre ...`
- `sched: aliasprobe task_base_post ...`
- `sched: aliasprobe task_off_ff8_post ...`

If panic occurs, include the full panic block with:
- FAR / ESR
- TTBR0 walk lines
- register dump and stack trace

## 3) Expected healthy signals (after fix)

For the same boot attempt:

1. `aliasprobe[pre_handles]` and `aliasprobe[post_handles]` both report:
   - `l1[511] ... valid=yes`
2. `task_off_ff8_*` probes report:
   - software physical == hardware physical
   - `fault=no`
3. Task and user L1 table pages are different physical pages.
4. No EL0 data abort at FAR `0x00007fffffefff80`.

## 4) Red-flag signals (possible remaining alias/corruption)

Any of the following is a failure signal:

- `l1[511]` flips from valid to invalid between pre/post handle init.
- `task_off_ff8` probe shows hw/sw translation mismatch.
- `fault=yes` for task probe VAs.
- Panic shows TTBR0 walk with missing `L1[511]` for user stack VA.

## 5) What to send back from a Pi run

Please provide:

1. Full serial log from boot through:
   - successful userspace init output, or
   - kernel panic block.
2. The 6 `aliasprobe` lines listed above.
3. If panic: complete panic section (not truncated).
